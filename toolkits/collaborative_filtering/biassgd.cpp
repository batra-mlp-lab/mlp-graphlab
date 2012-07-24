/**  
 * Copyright (c) 2009 Carnegie Mellon University. 
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://graphlab.org
 *
 */


/**
 * \file
 * \ingroup toolkit_matrix_pvecization
 *
 * \brief This file describes the vertex program for the alternating
 * least squares (BIASSGD) matrix pvecization algorithm.  See
 * \ref biassgd_vertex_program for description of the BIASSGD Algorithm.
 * This file contains the main body of the BIAS-SGD matrix factorization
 * algorithm. 
 */
#include <Eigen/Dense>

#include <boost/config/warning_disable.hpp>
#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/phoenix_core.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>
#include <boost/spirit/include/phoenix_stl.hpp>



#include <graphlab/util/stl_util.hpp>
#include "eigen_serialization.hpp"
#include <graphlab.hpp>


#include <graphlab/macros_def.hpp>



typedef Eigen::VectorXd vec_type;
typedef Eigen::MatrixXd mat_type;

bool debug;
int iter = 0;

/**
 * \brief Remap the target id of each edge into a different id space
 * than the source id.
 */
bool REMAP_TARGET = false;



/** 
 * \ingroup toolkit_matrix_pvecization
 *
 * \brief the vertex data type which contains the latent pvec.
 *
 * Each row and each column in the matrix corresponds to a different
 * vertex in the BIASSGD graph.  Associated with each vertex is a pvec
 * (vector) of latent parameters that represent that vertex.  The goal
 * of the BIASSGD algorithm is to find the values for these latent
 * parameters such that the non-zero entries in the matrix can be
 * predicted by taking the dot product of the row and column pvecs.
 */
struct vertex_data {
  /**
   * \brief A shared "constant" that specifies the number of latent
   * values to use.
   */
  static size_t NLATENT;
  /** \brief The number of times this vertex has been updated. */
  uint32_t nupdates;
  /** \brief The latent pvec for this vertex */
  vec_type pvec;
  vec_type weight;
  double bias;
  /** 
   * \brief Simple default constructor which randomizes the vertex
   *  data 
   */
  vertex_data() : nupdates(0) { if (debug) pvec = vec_type::Ones(NLATENT); else randomize(); } 
  /** \brief Randomizes the latent pvec */
  void randomize() { pvec.resize(NLATENT); pvec.setRandom(); }
  /** \brief Save the vertex data to a binary archive */
  void save(graphlab::oarchive& arc) const { 
    arc << nupdates << pvec << weight << bias;
  }
  /** \brief Load the vertex data from a binary archive */
  void load(graphlab::iarchive& arc) { 
    arc >> nupdates >> pvec >> weight >> bias;
  }
}; // end of vertex data


/**
 * \brief The edge data stores the entry in the matrix.
 *
 * In addition the edge data biassgdo stores the most recent error estimate.
 */
struct edge_data : public graphlab::IS_POD_TYPE {
  /**
   * \brief The type of data on the edge;
   *
   * \li *Train:* the observed value is correct and used in training
   * \li *Validate:* the observed value is correct but not used in training
   * \li *Predict:* The observed value is not correct and should not be
   *        used in training.
   */
  enum data_role_type { TRAIN, VALIDATE, PREDICT  };

  /** \brief the observed value for the edge */
  float obs;

  /** \brief The train/validation/test designation of the edge */
  data_role_type role;

  /** \brief basic initialization */
  edge_data(float obs = 0, data_role_type role = PREDICT) :
    obs(obs), role(role) { }

}; // end of edge data


/**
 * \brief The graph type is defined in terms of the vertex and edge
 * data.
 */ 
typedef graphlab::distributed_graph<vertex_data, edge_data> graph_type;
double extract_l2_error(const graph_type::edge_type & edge);


/**
 * \brief Given a vertex and an edge return the other vertex in the
 * edge.
 */
inline graph_type::vertex_type
get_other_vertex(graph_type::edge_type& edge, 
                 const graph_type::vertex_type& vertex) {
  return vertex.id() == edge.source().id()? edge.target() : edge.source();
}; // end of get_other_vertex





/**
 * \brief The gather type used to construct XtX and Xty needed for the BIASSGD
 * update
 *
 * To compute the ALS update we need to compute the sum of 
 * \code
 *  sum: XtX = nbr.pvec.transpose() * nbr.pvec 
 *  sum: Xy  = nbr.pvec * edge.obs
 * \endcode
 * For each of the neighbors of a vertex. 
 *
 * To do this in the Gather-Apply-Scatter model the gather function
 * computes and returns a pair consisting of XtX and Xy which are then
 * added. The gather type represents that tuple and provides the
 * necessary gather_type::operator+= operation.
 *
 */
class gather_type {
public:
  /**
   * \brief Stores the current sum of nbr.pvec.transpose() *
   * nbr.pvec
   */

  /**
   * \brief Stores the current sum of nbr.pvec * edge.obs
   */
  vec_type pvec;
  double bias;

  /** \brief basic default constructor */
  gather_type() { }

  /**
   * \brief This constructor computes XtX and Xy and stores the result
   * in XtX and Xy
   */
  gather_type(const vec_type& X, double _bias) {
    pvec = X;
    bias = _bias;
  } // end of constructor for gather type

  /** \brief Save the values to a binary archive */
  void save(graphlab::oarchive& arc) const { arc << pvec << bias; }

  /** \brief Read the values from a binary archive */
  void load(graphlab::iarchive& arc) { arc >> pvec >> bias; }  

  /** 
   * \brief Computes XtX += other.XtX and Xy += other.Xy updating this
   * tuples value
   */
  gather_type& operator+=(const gather_type& other) {
    if (pvec.size() == 0){
      pvec = other.pvec;
      bias = other.bias;
      return *this;
    }
    else if (other.pvec.size() == 0)
      return *this;
    pvec += other.pvec;
    bias += other.bias;
    return *this;
  } // end of operator+=

}; // end of gather type

//typedef gather_type message_type;

/**
 * BIASSGD vertex program type
 */ 
class biassgd_vertex_program : 
  public graphlab::ivertex_program<graph_type, gather_type,
                                   gather_type> {
public:
  /** The convergence tolerance */
  static double TOLERANCE;
  static double LAMBDA;
  static double GAMMA;
  static double MAXVAL;
  static double MINVAL;
  static double STEP_DEC;
  static bool debug;
  static size_t MAX_UPDATES;
  static double GLOBAL_MEAN;
  static size_t NUM_TRAINING_EDGES;

  gather_type pmsg;
  void save(graphlab::oarchive& arc) const { 
    arc << pmsg;
  }
  /** \brief Load the vertex data from a binary archive */
  void load(graphlab::iarchive& arc) { 
    arc >> pmsg;
  }

  /** The set of edges to gather along */
  edge_dir_type gather_edges(icontext_type& context, 
                             const vertex_type& vertex) const { 
    return graphlab::ALL_EDGES; 
  }; // end of gather_edges 

  /** The gather function computes XtX and Xy */
  gather_type gather(icontext_type& context, const vertex_type& vertex, 
                     edge_type& edge) const {
    //if(edge.data().role == edge_data::TRAIN) {
    vec_type delta, other_delta;
    double bias =0, other_bias = 0;

    if (vertex.num_in_edges() == 0){
      vertex_type other_vertex(get_other_vertex(edge, vertex));
      vertex_type my_vertex(vertex);
      //vertex_data & my_data = my_vertex.data();
      double pred = biassgd_vertex_program::GLOBAL_MEAN + 
        edge.source().data().bias + edge.target().data().bias + 
        vertex.data().pvec.dot(other_vertex.data().pvec);
      pred = std::min(pred, biassgd_vertex_program::MAXVAL);
      pred = std::max(pred, biassgd_vertex_program::MINVAL); 
      const float err = (pred - edge.data().obs);
      if (debug)
        std::cout <<"entering edge " << (int)edge.source().id() << ":" 
                  << (int)edge.target().id()-1000000 
                  << " err: " << err << " rmse: " << err*err <<std::endl;
      assert(!std::isnan(err));
      if (edge.data().role == edge_data::TRAIN){

        bias = -GAMMA*(err - LAMBDA*bias);
        other_bias = -GAMMA*(err - LAMBDA* other_bias);
         
        delta = -GAMMA*(err*other_vertex.data().pvec - LAMBDA*vertex.data().pvec);
        other_delta = -GAMMA*(err*vertex.data().pvec - LAMBDA*other_vertex.data().pvec);
       
        //A HACK: update memory cached values to reflect new vals 
        my_vertex.data().bias += bias;
        other_vertex.data().bias += other_bias;
        my_vertex.data().pvec += delta;
	other_vertex.data().pvec += other_delta;
      
        if (debug)
          std::cout << "new val:" << (int)edge.source().id() << ":" 
                    << (int)edge.target().id()-1000000 
                    << " U " << my_vertex.data().pvec.transpose() 
                    << " V " << other_vertex.data().pvec.transpose() << std::endl;
        if(other_vertex.data().nupdates < MAX_UPDATES) 
          context.signal(other_vertex, gather_type(other_delta, other_bias));
      }
      return gather_type(delta, bias);
    } 
    else return gather_type(delta, bias);
  } // end of gather function

  //typedef vec_type message_type;
  void init(icontext_type& context,
            const vertex_type& vertex,
            const message_type& msg) {
    if (vertex.num_in_edges() > 0){
      pmsg = msg;
    }
  }
  /** apply collects the sum of XtX and Xy */
  void apply(icontext_type& context, vertex_type& vertex,
             const gather_type& sum) {
    // Get and reset the vertex data
    vertex_data& vdata = vertex.data(); 
    if (sum.pvec.size() > 0){
      vdata.pvec += sum.pvec; 
      assert(vertex.num_in_edges() == 0);
    }
    else if (pmsg.pvec.size() > 0){
      vdata.pvec += pmsg.pvec;
      vdata.bias += pmsg.bias;
      assert(vertex.num_out_edges() == 0); 
    }
    ++vdata.nupdates;
  } // end of apply
  
  /** The edges to scatter along */
  edge_dir_type scatter_edges(icontext_type& context,
                              const vertex_type& vertex) const { 
    return graphlab::ALL_EDGES; 
  }; // end of scatter edges

  /** Scatter reschedules neighbors */  
  void scatter(icontext_type& context, const vertex_type& vertex, 
               edge_type& edge) const {
    edge_data& edata = edge.data();
    if(edata.role == edge_data::TRAIN) {
      const vertex_type other_vertex = get_other_vertex(edge, vertex);
      // Reschedule neighbors ------------------------------------------------
      if(other_vertex.data().nupdates < MAX_UPDATES) 
        context.signal(other_vertex, gather_type(vec_type::Zero(vertex_data::NLATENT),0));
    }
  } // end of scatter function


  /**
   * \brief Signal all vertices on one side of the bipartite graph
   */
  static graphlab::empty signal_left(icontext_type& context,
                                     vertex_type& vertex) {
    if(vertex.num_out_edges() > 0) context.signal(vertex, gather_type(vec_type::Zero(vertex_data::NLATENT),0));
    return graphlab::empty();
  } // end of signal_left 

}; // end of biassgd vertex program


struct error_aggregator : public graphlab::IS_POD_TYPE {
  typedef biassgd_vertex_program::icontext_type icontext_type;
  typedef graph_type::edge_type edge_type;
  double train_error, validation_error;
  size_t ntrain, nvalidation;
  error_aggregator() : 
    train_error(0), validation_error(0), ntrain(0), nvalidation(0) { }
  error_aggregator& operator+=(const error_aggregator& other) {
    train_error += other.train_error;
    assert(!std::isnan(train_error));
    validation_error += other.validation_error;
    ntrain += other.ntrain;
    nvalidation += other.nvalidation;
    return *this;
  }
  static error_aggregator map(icontext_type& context, const graph_type::edge_type& edge) {
    error_aggregator agg;
    if (edge.data().role == edge_data::TRAIN){
      agg.train_error = extract_l2_error(edge); agg.ntrain = 1;
      assert(!std::isnan(agg.train_error));
    }
    else if (edge.data().role == edge_data::VALIDATE){
      agg.validation_error = extract_l2_error(edge); agg.nvalidation = 1;
    }
    return agg;
  }


  static void finalize(icontext_type& context, const error_aggregator& agg) {
    iter++;
    if (iter%2 == 0)
      return; 
    ASSERT_GT(agg.ntrain, 0);
    const double train_error = std::sqrt(agg.train_error / agg.ntrain);
    assert(!std::isnan(train_error));
    context.cout() << context.elapsed_seconds() << "\t" << train_error;
    if(agg.nvalidation > 0) {
      const double validation_error = 
        std::sqrt(agg.validation_error / agg.nvalidation);
      context.cout() << "\t" << validation_error; 
    }
    context.cout() << std::endl;
    biassgd_vertex_program::GAMMA *= biassgd_vertex_program::STEP_DEC;
  }
}; // end of error aggregator

/**
 * \brief Given an edge compute the error associated with that edge
 */
double extract_l2_error(const graph_type::edge_type & edge) {
  double pred = biassgd_vertex_program::GLOBAL_MEAN + 
    edge.source().data().bias +
    edge.target().data().bias + 
    edge.source().data().pvec.dot(edge.target().data().pvec);
  pred = std::min(biassgd_vertex_program::MAXVAL, pred);
  pred = std::max(biassgd_vertex_program::MINVAL, pred);
  double rmse = (edge.data().obs - pred) * (edge.data().obs - pred);
  assert(rmse <= pow(biassgd_vertex_program::MAXVAL-biassgd_vertex_program::MINVAL,2));
  return rmse;
} // end of extract_l2_error


/**
 * \brief The prediction saver is used by the graph.save routine to
 * output the final predictions back to the filesystem.
 */
struct prediction_saver {
  typedef graph_type::vertex_type vertex_type;
  typedef graph_type::edge_type   edge_type;
  std::string save_vertex(const vertex_type& vertex) const {
    return ""; //nop
  }
  std::string save_edge(const edge_type& edge) const {
    std::stringstream strm;
    const double prediction = 
      edge.source().data().pvec.dot(edge.target().data().pvec);
    strm << edge.source().id() << '\t';
    if(REMAP_TARGET) strm << (-edge.target().id() - 2) << '\t';
    else strm << edge.target().id() << '\t';
    strm << prediction << '\n';
    return strm.str();
  }
}; // end of prediction_saver



/**
 * \brief The graph loader function is a line parser used for
 * distributed graph construction.
 */
inline bool graph_loader(graph_type& graph, 
                         const std::string& filename,
                         const std::string& line) {
  ASSERT_FALSE(line.empty()); 
  namespace qi = boost::spirit::qi;
  namespace ascii = boost::spirit::ascii;
  namespace phoenix = boost::phoenix;
  // Determine the role of the data
  edge_data::data_role_type role = edge_data::TRAIN;
  if(boost::ends_with(filename,".validate")) role = edge_data::VALIDATE;
  else if(boost::ends_with(filename, ".predict")) role = edge_data::PREDICT;
  // Parse the line
  graph_type::vertex_id_type source_id(-1), target_id(-1);
  float obs(0); 
  const bool success = qi::phrase_parse
    (line.begin(), line.end(),       
     //  Begin grammar
     (
      qi::ulong_[phoenix::ref(source_id) = qi::_1] >> -qi::char_(',') >>
      qi::ulong_[phoenix::ref(target_id) = qi::_1] >> 
      -(-qi::char_(',') >> qi::float_[phoenix::ref(obs) = qi::_1])
      )
     ,
     //  End grammar
     ascii::space); 
  if(!success) return false;
  if(REMAP_TARGET) {
    // map target id into a separate number space
    target_id = -(graphlab::vertex_id_type(target_id + 2));
  }
  // Create an edge and add it to the graph
  graph.add_edge(source_id, target_id, edge_data(obs, role)); 
  return true; // successful load
} // end of graph_loader



size_t vertex_data::NLATENT = 20;
double biassgd_vertex_program::TOLERANCE = 1e-3;
double biassgd_vertex_program::LAMBDA = 0.001;
double biassgd_vertex_program::GAMMA = 0.001;
size_t biassgd_vertex_program::MAX_UPDATES = -1;
double biassgd_vertex_program::MAXVAL = 1e+100;
double biassgd_vertex_program::MINVAL = 1e-100;
double biassgd_vertex_program::STEP_DEC = 0.9;
bool biassgd_vertex_program::debug = false;
double biassgd_vertex_program::GLOBAL_MEAN = 0;
size_t biassgd_vertex_program::NUM_TRAINING_EDGES = 0;

/**
 * \brief The engine type used by the ALS matrix factorization
 * algorithm.
 *
 * The ALS matrix factorization algorithm currently uses the
 * synchronous engine.  However we plan to add support for alternative
 * engines in the future.
 */
typedef graphlab::omni_engine<biassgd_vertex_program> engine_type;

double calc_global_mean(const graph_type::edge_type & edge){
  if (edge.data().role == edge_data::TRAIN)
    return edge.data().obs;
  else return 0;
}

size_t count_edges(const graph_type::edge_type & edge){
  if (edge.data().role == edge_data::TRAIN)
    return 1;
  else return 0;
}


int main(int argc, char** argv) {
  global_logger().set_log_level(LOG_INFO);
  global_logger().set_log_to_console(true);

  // Parse command line options -----------------------------------------------
  const std::string description = 
    "Compute the ALS factorization of a matrix.";
  graphlab::command_line_options clopts(description);
  std::string input_dir, output_dir;
  std::string predictions;
  size_t interval = 0;
  std::string exec_type = "synchronous";
  clopts.attach_option("matrix", input_dir,
                       "The directory containing the matrix file");
  clopts.add_positional("matrix");
  clopts.attach_option("D", vertex_data::NLATENT,
                       "Number of latent parameters to use.");
  clopts.attach_option("engine", exec_type, 
                       "The engine type synchronous or asynchronous");
  clopts.attach_option("max_iter", biassgd_vertex_program::MAX_UPDATES,
                       "The maxumum number of udpates allowed for a vertex");
  clopts.attach_option("lambda", biassgd_vertex_program::LAMBDA, 
                       "SGD regularization weight"); 
  clopts.attach_option("gamma", biassgd_vertex_program::GAMMA, 
                       "SGD step size"); 
  clopts.attach_option("debug", biassgd_vertex_program::debug, 
                       "debug - additional verbose info"); 
  clopts.attach_option("tol", biassgd_vertex_program::TOLERANCE,
                       "residual termination threshold");
  clopts.attach_option("maxval", biassgd_vertex_program::MAXVAL, "max allowed value");
  clopts.attach_option("minval", biassgd_vertex_program::MINVAL, "min allowed value");
  clopts.attach_option("step_dec", biassgd_vertex_program::STEP_DEC, "multiplicative step decrement");
  clopts.attach_option("interval", interval, 
                       "The time in seconds between error reports");
  clopts.attach_option("predictions", predictions,
                       "The prefix (folder and filename) to save predictions.");
  clopts.attach_option("output", output_dir,
                       "Output results");
  if(!clopts.parse(argc, argv)) {
    std::cout << "Error in parsing command line arguments." << std::endl;
    return EXIT_FAILURE;
  }
  debug = biassgd_vertex_program::debug;
  //  omp_set_num_threads(clopts.get_ncpus());
  ///! Initialize control plain using mpi
  graphlab::mpi_tools::init(argc, argv);
  graphlab::distributed_control dc;
  
  dc.cout() << "Loading graph." << std::endl;
  graphlab::timer timer; 
  graph_type graph(dc, clopts);  
  graph.load(input_dir, graph_loader); 
  dc.cout() << "Loading graph. Finished in " 
            << timer.current_time() << std::endl;
  dc.cout() << "Finalizing graph." << std::endl;
  timer.start();
  graph.finalize();
  dc.cout() << "Finalizing graph. Finished in " 
            << timer.current_time() << std::endl;


  dc.cout() 
    << "========== Graph statistics on proc " << dc.procid() 
    << " ==============="
    << "\n Num vertices: " << graph.num_vertices()
    << "\n Num edges: " << graph.num_edges()
    << "\n Num replica: " << graph.num_replicas()
    << "\n Replica to vertex ratio: " 
    << float(graph.num_replicas())/graph.num_vertices()
    << "\n --------------------------------------------" 
    << "\n Num local own vertices: " << graph.num_local_own_vertices()
    << "\n Num local vertices: " << graph.num_local_vertices()
    << "\n Replica to own ratio: " 
    << (float)graph.num_local_vertices()/graph.num_local_own_vertices()
    << "\n Num local edges: " << graph.num_local_edges()
    //<< "\n Begin edge id: " << graph.global_eid(0)
    << "\n Edge balance ratio: " 
    << float(graph.num_local_edges())/graph.num_edges()
    << std::endl;
 
  dc.cout() << "Creating engine" << std::endl;
  engine_type engine(dc, graph, exec_type, clopts);

  // Add error reporting to the engine
  const bool success = engine.add_edge_aggregator<error_aggregator>
    ("error", error_aggregator::map, error_aggregator::finalize) &&
    engine.aggregate_periodic("error", interval);
  ASSERT_TRUE(success);
  

  biassgd_vertex_program::GLOBAL_MEAN = graph.map_reduce_edges<double>(calc_global_mean);
  biassgd_vertex_program::NUM_TRAINING_EDGES = graph.map_reduce_edges<size_t>(count_edges);
  biassgd_vertex_program::GLOBAL_MEAN /= biassgd_vertex_program::NUM_TRAINING_EDGES;
  dc.cout() << "Global mean is: " <<biassgd_vertex_program::GLOBAL_MEAN << std::endl;

  // Signal all vertices on the vertices on the left (libersgd) 
  engine.map_reduce_vertices<graphlab::empty>(biassgd_vertex_program::signal_left);
 

  // Run the PageRank ---------------------------------------------------------
  dc.cout() << "Running Bias-SGD" << std::endl;
  timer.start();
  engine.start();  

  const double runtime = timer.current_time();
  dc.cout() << "----------------------------------------------------------"
            << std::endl
            << "Final Runtime (seconds):   " << runtime 
            << std::endl
            << "Updates executed: " << engine.num_updates() << std::endl
            << "Update Rate (updates/second): " 
            << engine.num_updates() / runtime << std::endl;

  // Compute the final training error -----------------------------------------
  dc.cout() << "Final error: " << std::endl;
  engine.aggregate_now("error");

  // Make predictions ---------------------------------------------------------
  if(!predictions.empty()) {
    std::cout << "Saving predictions" << std::endl;
    const bool gzip_output = false;
    const bool save_vertices = false;
    const bool save_edges = true;
    const size_t threads_per_machine = 2;
    graph.save(predictions, prediction_saver(),
               gzip_output, save_vertices, 
               save_edges, threads_per_machine);
    
  }
             


  graphlab::mpi_tools::finalize();
  return EXIT_SUCCESS;
} // end of main



