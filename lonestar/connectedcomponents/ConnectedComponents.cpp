/** Connected components -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section Description
 *
 * Compute the connect components of a graph and optionally write out the largest
 * component to file.
 *
 * @author Donald Nguyen <ddn@cs.utexas.edu>
 */
#include "galois/Galois.h"
#include "galois/Accumulator.h"
#include "galois/Bag.h"
#include "galois/DomainSpecificExecutors.h"
#include "galois/Timer.h"
#include "galois/UnionFind.h"
#include "galois/graphs/LCGraph.h"
#include "galois/graphs/OCGraph.h"
#include "galois/graphs/TypeTraits.h"
#include "galois/ParallelSTL.h"
#include "llvm/Support/CommandLine.h"
#include "Lonestar/BoilerPlate.h"

#include <utility>
#include <vector>
#include <algorithm>
#include <iostream>

#ifdef GALOIS_USE_EXP
#include "LigraAlgo.h"
#include "GraphLabAlgo.h"
#include "GraphChiAlgo.h"
#endif

#include <ostream>
#include <fstream>

const char* name = "Connected Components";
const char* desc = "Computes the connected components of a graph";
const char* url = 0;

enum Algo {
  async,
  asyncOc,
  blockedasync,
  graphchi,
  graphlab,
  labelProp,
  ligra,
  ligraChi,
  serial,
  synchronous
};

enum OutputEdgeType {
  void_,
  int32_,
  int64_
};

namespace cll = llvm::cl;
static cll::opt<std::string> inputFilename(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<std::string> largestComponentFilename("outputLargestComponent", cll::desc("[output graph file]"), cll::init(""));
static cll::opt<std::string> permutationFilename("outputNodePermutation", cll::desc("[output node permutation file]"), cll::init(""));
static cll::opt<std::string> transposeGraphName("graphTranspose", cll::desc("Transpose of input graph"));
static cll::opt<bool> symmetricGraph("symmetricGraph", cll::desc("Input graph is symmetric"), cll::init(false));
cll::opt<unsigned int> memoryLimit("memoryLimit",
    cll::desc("Memory limit for out-of-core algorithms (in MB)"), cll::init(~0U));
static cll::opt<OutputEdgeType> writeEdgeType("edgeType", cll::desc("Input/Output edge type:"),
    cll::values(
      clEnumValN(OutputEdgeType::void_, "void", "no edge values"),
      clEnumValN(OutputEdgeType::int32_, "int32", "32 bit edge values"),
      clEnumValN(OutputEdgeType::int64_, "int64", "64 bit edge values"),
      clEnumValEnd), cll::init(OutputEdgeType::void_));
static cll::opt<Algo> algo("algo", cll::desc("Choose an algorithm:"),
    cll::values(
      clEnumValN(Algo::async, "async", "Asynchronous (default)"),
      clEnumValN(Algo::blockedasync, "blockedasync", "Blocked asynchronous"),
      clEnumValN(Algo::asyncOc, "asyncOc", "Asynchronous out-of-core memory"),
      clEnumValN(Algo::labelProp, "labelProp", "Using label propagation algorithm"),
      clEnumValN(Algo::serial, "serial", "Serial"),
      clEnumValN(Algo::synchronous, "sync", "Synchronous"),
#ifdef GALOIS_USE_EXP
      clEnumValN(Algo::graphchi, "graphchi", "Using GraphChi programming model"),
      clEnumValN(Algo::graphlab, "graphlab", "Using GraphLab programming model"),
      clEnumValN(Algo::ligraChi, "ligraChi", "Using Ligra and GraphChi programming model"),
      clEnumValN(Algo::ligra, "ligra", "Using Ligra programming model"),
#endif
      clEnumValEnd), cll::init(Algo::async));

struct Node: public galois::UnionFindNode<Node> {
  typedef Node* component_type;
  unsigned int id;

  Node(): galois::UnionFindNode<Node>(const_cast<Node*>(this)) { }
  Node(const Node& o): galois::UnionFindNode<Node>(o.m_component), id(o.id) { }

  Node& operator=(const Node& o) {
    Node c(o);
    std::swap(c, *this);
    return *this;
  }

  component_type component() { return this->findAndCompress(); }
};

template<typename Graph>
void readInOutGraph(Graph& graph) {
  using namespace galois::graphs;
  if (symmetricGraph) {
    galois::graphs::readGraph(graph, inputFilename);
  } else if (transposeGraphName.size()) {
    galois::graphs::readGraph(graph, inputFilename, transposeGraphName);
  } else {
    GALOIS_DIE("Graph type not supported");
  }
}

/** 
 * Serial connected components algorithm. Just use union-find.
 */
struct SerialAlgo {
  typedef galois::graphs::LC_CSR_Graph<Node,void>
    ::with_no_lockable<true>::type Graph;
  typedef Graph::GraphNode GNode;

  template<typename G>
  void readGraph(G& graph) { galois::graphs::readGraph(graph, inputFilename); }

  void operator()(Graph& graph) {

    for (const GNode& src: graph) {

      Node& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);

      for (auto ii : graph.edges(src, galois::MethodFlag::UNPROTECTED)) {
        GNode dst = graph.getEdgeDst(ii);
        Node& ddata = graph.getData(dst, galois::MethodFlag::UNPROTECTED);
        sdata.merge(&ddata);
      }
    }

  }
};

/**
 * Synchronous connected components algorithm.  Initially all nodes are in
 * their own component. Then, we merge endpoints of edges to form the spanning
 * tree. Merging is done in two phases to simplify concurrent updates: (1)
 * find components and (2) union components.  Since the merge phase does not
 * do any finds, we only process a fraction of edges at a time; otherwise,
 * the union phase may unnecessarily merge two endpoints in the same
 * component.
 */
struct SynchronousAlgo {
  typedef galois::graphs::LC_CSR_Graph<Node,void>
    ::with_no_lockable<true>::type
    ::with_numa_alloc<true>::type Graph;
  typedef Graph::GraphNode GNode;

  template<typename G>
  void readGraph(G& graph) { galois::graphs::readGraph(graph, inputFilename); }

  struct Edge {
    GNode src;
    Node* ddata;
    int count;
    Edge(GNode src, Node* ddata, int count): src(src), ddata(ddata), count(count) { }
  };

  void operator()(Graph& graph) {
    size_t rounds;
    galois::GAccumulator<size_t> emptyMerges;

    galois::InsertBag<Edge> wls[2];
    galois::InsertBag<Edge>* next;
    galois::InsertBag<Edge>* cur;

    cur = &wls[0];
    next = &wls[1];

    galois::do_all(galois::iterate(graph), 
        [&] (const GNode& src) {
          for (auto ii : graph.edges(src, galois::MethodFlag::UNPROTECTED)) {
            GNode dst = graph.getEdgeDst(ii);
            if (symmetricGraph && src >= dst)
              continue;
            Node& ddata = graph.getData(dst, galois::MethodFlag::UNPROTECTED);
            next->push(Edge(src, &ddata, 0));
            break;
          }
        },
        galois::no_stats());
        

    while (!cur->empty()) {

      galois::do_all(galois::iterate(*cur), 
          [&] (const Edge& edge) {
            Node& sdata = graph.getData(edge.src, galois::MethodFlag::UNPROTECTED);
            if (!sdata.merge(edge.ddata))
              emptyMerges += 1;

          },
          galois::timeit(),
          galois::loopname("Merge"));
          

      galois::do_all(galois::iterate(*cur), 
          [&] (const Edge& edge) {
            GNode src = edge.src;
            Node& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);
            Node* scomponent = sdata.findAndCompress();
            Graph::edge_iterator ii = graph.edge_begin(src, galois::MethodFlag::UNPROTECTED);
            Graph::edge_iterator ei = graph.edge_end(src, galois::MethodFlag::UNPROTECTED);
            int count = edge.count + 1;
            std::advance(ii, count);
            for (; ii != ei; ++ii, ++count) {
              GNode dst = graph.getEdgeDst(ii);
              if (symmetricGraph && src >= dst)
                continue;
              Node& ddata = graph.getData(dst, galois::MethodFlag::UNPROTECTED);
              Node* dcomponent = ddata.findAndCompress();
              if (scomponent != dcomponent) {
                next->push(Edge(src, dcomponent, count));
                break;
              }
            }
          },
          galois::timeit(),
          galois::loopname("Find"));


      cur->clear();
      std::swap(cur, next);
      rounds += 1;
    }

    galois::runtime::reportStat_Single("CC-Sync", "rounds", rounds);
    galois::runtime::reportStat_Single("CC-Sync", "emptyMerges", emptyMerges.reduce());
  }
};

struct LabelPropAlgo {
  struct LNode {
    typedef unsigned int component_type;
    unsigned int id;
    unsigned int comp;
    
    component_type component() { return comp; }
    bool isRep() { return id == comp; }
  };

  typedef galois::graphs::LC_CSR_Graph<LNode,void>
    ::with_no_lockable<true>::type
    ::with_numa_alloc<true>::type InnerGraph;
  typedef galois::graphs::LC_InOut_Graph<InnerGraph> Graph;
  typedef Graph::GraphNode GNode;
  typedef LNode::component_type component_type;

  template<typename G>
  void readGraph(G& graph) {
    readInOutGraph(graph);
  }

  template<typename C>
  static void update(Graph& graph , LNode& sdata, const GNode& dst, C& ctx) {
    LNode& ddata = graph.getData(dst, galois::MethodFlag::UNPROTECTED);

    while (true) {
      component_type old = ddata.comp;
      component_type newV = sdata.comp;
      if (old <= newV)
        break;
      if (__sync_bool_compare_and_swap(&ddata.comp, old, newV)) {
        ctx.push(dst);
        break;
      }
    }
  }


  void operator()(Graph& graph) {
    typedef galois::worklists::dChunkedFIFO<256> WL;

    galois::do_all(galois::iterate(graph), 
        [&] (const GNode& n) {
          LNode& data = graph.getData(n, galois::MethodFlag::UNPROTECTED);
          data.comp = data.id;
        },
        galois::loopname("Initialize"));

    if (symmetricGraph) {
      galois::for_each(galois::iterate(graph), 
          [&] (const GNode& src, auto& ctx) {

            LNode& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);

            for (auto e: graph.edges(src, galois::MethodFlag::UNPROTECTED)) {
              update(graph, sdata, graph.getEdgeDst(e), ctx);
            }

          },
          galois::no_conflicts(),
          galois::loopname("LabelPropAlgo"),
          galois::wl<WL>());

    } else {

      galois::for_each(galois::iterate(graph), 
          [&] (const GNode& src, auto& ctx) {

            LNode& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);

            for (auto e: graph.in_edges(src, galois::MethodFlag::UNPROTECTED)) {
              update(graph, sdata, graph.getInEdgeDst(e), ctx);
            }

            for (auto e: graph.edges(src, galois::MethodFlag::UNPROTECTED)) {
              update(graph, sdata, graph.getEdgeDst(e), ctx);
            }

          },
          galois::no_conflicts(),
          galois::loopname("LabelPropAlgo"),
          galois::wl<WL>());
    }
  }
};

struct AsyncOCAlgo {
  typedef galois::graphs::OCImmutableEdgeGraph<Node,void> Graph;
  typedef Graph::GraphNode GNode;

  template<typename G>
  void readGraph(G& graph) {
    readInOutGraph(graph);
  }

  void operator()(Graph& graph) {

    galois::GAccumulator<size_t> emptyMerges;
    
    galois::graphChi::vertexMap(graph, 
        [&emptyMerges] (auto& g, const GNode& src) {
          Node& sdata = g.getData(src, galois::MethodFlag::UNPROTECTED);

          for (auto ii : g.edges(src, galois::MethodFlag::UNPROTECTED)) {
            GNode dst = g.getEdgeDst(ii);
            Node& ddata = g.getData(dst, galois::MethodFlag::UNPROTECTED);

            if (symmetricGraph && src >= dst)
              continue;

            if (!sdata.merge(&ddata))
              emptyMerges += 1;
          }
        },
        memoryLimit);

    galois::runtime::reportStat_Single("CC-GraphChi", "emptyMerges", emptyMerges.reduce());
  }
};

/**
 * Like synchronous algorithm, but if we restrict path compression (as done is
 * @link{UnionFindNode}), we can perform unions and finds concurrently.
 */
struct AsyncAlgo {
  typedef galois::graphs::LC_CSR_Graph<Node,void>
    ::with_numa_alloc<true>::type
    ::with_no_lockable<true>::type
    Graph;
  typedef Graph::GraphNode GNode;

  template<typename G>
  void readGraph(G& graph) { galois::graphs::readGraph(graph, inputFilename); }

  void operator()(Graph& graph) {
    galois::GAccumulator<size_t> emptyMerges;

    galois::do_all(galois::iterate(graph), 
        [&] (const GNode& src) {
          Node& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);

          for (auto ii : graph.edges(src, galois::MethodFlag::UNPROTECTED)) {
            GNode dst = graph.getEdgeDst(ii);
            Node& ddata = graph.getData(dst, galois::MethodFlag::UNPROTECTED);

            if (symmetricGraph && src >= dst)
              continue;

            if (!sdata.merge(&ddata))
              emptyMerges += 1;
          }
        },
        galois::loopname("CC-Async"));


    galois::runtime::reportStat_Single("CC-Async", "emptyMerges", emptyMerges.reduce());
  }
};

/**
 * Improve performance of async algorithm by following machine topology.
 */
struct BlockedAsyncAlgo {
  typedef galois::graphs::LC_CSR_Graph<Node,void>
    ::with_numa_alloc<true>::type
    ::with_no_lockable<true>::type
    Graph;
  typedef Graph::GraphNode GNode;

  struct WorkItem {
    GNode src;
    Graph::edge_iterator start;
  };

  template<typename G>
  void readGraph(G& graph) { galois::graphs::readGraph(graph, inputFilename); }

  //! Add the next edge between components to the worklist
  template<bool MakeContinuation, int Limit, typename Pusher>
  static void process(Graph& graph, const GNode& src, const Graph::edge_iterator& start, Pusher& pusher) {

    Node& sdata = graph.getData(src, galois::MethodFlag::UNPROTECTED);
    int count = 1;
    for (Graph::edge_iterator ii = start, ei = graph.edge_end(src, galois::MethodFlag::UNPROTECTED);
        ii != ei; 
        ++ii, ++count) {
      GNode dst = graph.getEdgeDst(ii);
      Node& ddata = graph.getData(dst, galois::MethodFlag::UNPROTECTED);

      if (symmetricGraph && src >= dst)
        continue;

      if (sdata.merge(&ddata)) {
        if (Limit == 0 || count != Limit)
          continue;
      }

      if (MakeContinuation || (Limit != 0 && count == Limit)) {
        WorkItem item = { src, ii + 1 };
        pusher.push(item);
        break;
      }
    }
  }

  void operator()(Graph& graph) {
    galois::InsertBag<WorkItem> items;

    galois::do_all(galois::iterate(graph), 
        [&] (const GNode& src) {
          Graph::edge_iterator start = graph.edge_begin(src, galois::MethodFlag::UNPROTECTED);
          if (galois::substrate::ThreadPool::getPackage() == 0) {
            process<true, 0>(graph, src, start, items);
          } else {
            process<true, 1>(graph, src, start, items);
          }
        },
        galois::loopname("Initialize"));

    galois::for_each(galois::iterate(items), 
        [&] (const WorkItem& item, auto& ctx) {
          process<true, 0>(graph, item.src, item.start, ctx);
        },
        galois::loopname("Merge"), galois::wl<galois::worklists::dChunkedFIFO<128> >());
  }
};


template<typename Graph>
bool verify(Graph& graph,
    typename std::enable_if<galois::graphs::is_segmented<Graph>::value>::type* = 0) {
  return true;
}

template<typename Graph>
bool verify(Graph& graph,
    typename std::enable_if<!galois::graphs::is_segmented<Graph>::value>::type* = 0) {

  using GNode = typename Graph::GraphNode;

  auto is_bad = [&graph] (const GNode& n) {

    auto& me = graph.getData(n);
    for (auto ii : graph.edges(n)) {
      GNode dst = graph.getEdgeDst(ii);
      auto& data = graph.getData(dst);
      if (data.component() != me.component()) {
        std::cerr << "not in same component: "
          << me.id << " (" << me.component() << ")"
          << " and "
          << data.id << " (" << data.component() << ")"
          << "\n";
        return true;
      }
    }
    return false;
  };

  return galois::ParallelSTL::find_if(graph.begin(), graph.end(), is_bad) == graph.end();
}

template<typename EdgeTy, typename Algo, typename CGraph>
void writeComponent(Algo& algo, CGraph& cgraph, typename CGraph::node_data_type::component_type component) {
  typedef typename CGraph::template with_edge_data<EdgeTy>::type Graph;

  if (std::is_same<Graph,CGraph>::value) {
    doWriteComponent(cgraph, component);
  } else {
    // copy node data from cgraph
    Graph graph;
    algo.readGraph(graph);
    typename Graph::iterator cc = graph.begin();
    for (typename CGraph::iterator ii = cgraph.begin(), ei = cgraph.end(); ii != ei; ++ii, ++cc) {
      graph.getData(*cc) = cgraph.getData(*ii);
    }
    doWriteComponent(graph, component);
  }
}

template<typename Graph>
void doWriteComponent(Graph& graph, typename Graph::node_data_type::component_type component,
    typename std::enable_if<galois::graphs::is_segmented<Graph>::value>::type* = 0) {
  GALOIS_DIE("Writing component not supported for this graph");
}

template<typename Graph>
void doWriteComponent(Graph& graph, typename Graph::node_data_type::component_type component,
    typename std::enable_if<!galois::graphs::is_segmented<Graph>::value>::type* = 0) {
  typedef typename Graph::GraphNode GNode;
  typedef typename Graph::node_data_reference node_data_reference;

  // set id to 1 if node is in component
  size_t numEdges = 0;
  size_t numNodes = 0;
  for (typename Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei; ++ii) {
    node_data_reference data = graph.getData(*ii);
    data.id = data.component() == component ? 1 : 0;
    if (data.id) {
      size_t degree = std::distance(graph.edge_begin(*ii), graph.edge_end(*ii));
      numEdges += degree;
      numNodes += 1;
    }
  }

  typedef galois::graphs::FileGraphWriter Writer;
  typedef galois::LargeArray<typename Graph::edge_data_type> EdgeData;
  typedef typename EdgeData::value_type edge_value_type;

  Writer p;
  EdgeData edgeData;
  p.setNumNodes(numNodes);
  p.setNumEdges(numEdges);
  p.setSizeofEdgeData(EdgeData::has_value ? sizeof(edge_value_type) : 0); 
  edgeData.create(numEdges);

  p.phase1();
  // partial sums of ids: id == new_index + 1
  typename Graph::node_data_type* prev = 0;
  for (typename Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei; ++ii) {
    node_data_reference data = graph.getData(*ii);
    if (prev)
      data.id = prev->id + data.id;
    if (data.component() == component) {
      size_t degree = std::distance(graph.edge_begin(*ii), graph.edge_end(*ii));
      size_t sid = data.id - 1;
      assert(sid < numNodes);
      p.incrementDegree(sid, degree);
    }
    
    prev = &data;
  }

  assert(!prev || prev->id == numNodes);

  if (largestComponentFilename != "") {
    p.phase2();
    for (auto ii : graph) {
      node_data_reference data = graph.getData(ii);
      if (data.component() != component)
        continue;

      size_t sid = data.id - 1;

      for (auto jj : graph.edges(ii)) {
        GNode dst = graph.getEdgeDst(jj);
        node_data_reference ddata = graph.getData(dst);
        size_t did = ddata.id - 1;

        //assert(ddata.component == component);
        assert(sid < numNodes && did < numNodes);
        if (EdgeData::has_value)
          edgeData.set(p.addNeighbor(sid, did), graph.getEdgeData(jj));
        else
          p.addNeighbor(sid, did);
      }
    }

    edge_value_type* rawEdgeData = p.finish<edge_value_type>();
    if (EdgeData::has_value)
      std::uninitialized_copy(std::make_move_iterator(edgeData.begin()), std::make_move_iterator(edgeData.end()), rawEdgeData);

    std::cout
      << "Writing largest component to " << largestComponentFilename
      << " (nodes: " << numNodes << " edges: " << numEdges << ")\n";

    p.toFile(largestComponentFilename);
  }

  if (permutationFilename != "") {
    std::ofstream out(permutationFilename);
    size_t oid = 0;
    std::cout << "Writing permutation to " << permutationFilename << "\n";
    for (typename Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei; ++ii, ++oid) {
      node_data_reference data = graph.getData(*ii);
      out << oid << ",";
      if (data.component() != component) {
        ;
      } else {
        out << data.id - 1;
      }
      out << "\n";
    }
  }
}


template<typename Graph>
typename Graph::node_data_type::component_type findLargest(Graph& graph) {

  using GNode = typename Graph::GraphNode;
  using component_type = typename Graph::node_data_type::component_type;
  using Map = galois::gstl::Map<component_type, int>;
  using ComponentSizePair = typename Map::value_type;
  
  galois::GMapElementAccumulator<Map> accumMap;
  galois::GAccumulator<size_t> accumReps;

  galois::do_all(galois::iterate(graph), 
      [&] (const GNode& x) {
        auto& n = graph.getData(x, galois::MethodFlag::UNPROTECTED);

        if (n.isRep()) {
          accumReps += 1;
          return;
        }

        // Don't add reps to table to avoid adding components of size 1
        accumMap.update(n.component(), 1);
      },
      galois::loopname("CountLargest"));

  Map& map = accumMap.reduce();
  size_t reps = accumReps.reduce();

  auto sizeMax = [] (const ComponentSizePair& a, const ComponentSizePair& b) {
    if (a.second > b.second) { 
      return a;
    }
    return b;
  };

  using MaxComp = galois::GSimpleReducible<ComponentSizePair, decltype(sizeMax)>;
  MaxComp maxComp(sizeMax);


  galois::do_all(galois::iterate(map), 
      [&] (const ComponentSizePair& x) {
        maxComp.update(x);
      },
      galois::no_stats());

  ComponentSizePair largest = maxComp.reduce();

  // Compensate for dropping representative node of components
  double ratio = graph.size() - reps + map.size();
  size_t largestSize = largest.second + 1;
  if (ratio)
    ratio = largestSize / ratio;

  std::cout << "Total components: " << reps << "\n";
  std::cout << "Number of non-trivial components: " << map.size()
    << " (largest size: " << largestSize << " [" << ratio << "])\n";

  return largest.first;
}

template<typename Algo>
void run() {
  typedef typename Algo::Graph Graph;

  Algo algo;
  Graph graph;

  algo.readGraph(graph);
  std::cout << "Read " << graph.size() << " nodes\n";

  unsigned int id = 0;
  for (typename Graph::iterator ii = graph.begin(), ei = graph.end(); ii != ei; ++ii, ++id) {
    graph.getData(*ii).id = id;
  }
  
  galois::preAlloc(numThreads + (2 * graph.size() * sizeof(typename Graph::node_data_type)) / galois::runtime::pagePoolSize());
  galois::reportPageAlloc("MeminfoPre");

  galois::StatTimer T;
  T.start();
  algo(graph);
  T.stop();

  galois::reportPageAlloc("MeminfoPost");

  if (!skipVerify || largestComponentFilename != "" || permutationFilename != "") {
    auto component = findLargest(graph);
    if (!verify(graph)) {
      GALOIS_DIE("verification failed");
    }
    if (component && (largestComponentFilename != "" || permutationFilename != "")) {
      switch (writeEdgeType) {
        case OutputEdgeType::void_: writeComponent<void>(algo, graph, component); break;
        case OutputEdgeType::int32_: writeComponent<uint32_t>(algo, graph, component); break;
        case OutputEdgeType::int64_: writeComponent<uint64_t>(algo, graph, component); break;
        default: abort();
      }
    }
  }
}

int main(int argc, char** argv) {
  galois::SharedMemSys G;
  LonestarStart(argc, argv, name, desc, url);

  galois::StatTimer T("TotalTime");
  T.start();
  switch (algo) {
    case Algo::asyncOc: run<AsyncOCAlgo>(); break;
    case Algo::async: run<AsyncAlgo>(); break;
    case Algo::blockedasync: run<BlockedAsyncAlgo>(); break;
    case Algo::labelProp: run<LabelPropAlgo>(); break;
    case Algo::serial: run<SerialAlgo>(); break;
    case Algo::synchronous: run<SynchronousAlgo>(); break;
#ifdef GALOIS_USE_EXP
    case Algo::graphchi: run<GraphChiAlgo>(); break;
    case Algo::graphlab: run<GraphLabAlgo>(); break;
    case Algo::ligraChi: run<LigraAlgo<true> >(); break;
    case Algo::ligra: run<LigraAlgo<false> >(); break;
#endif
    default: std::cerr << "Unknown algorithm\n"; abort();
  }
  T.stop();

  return 0;
}
