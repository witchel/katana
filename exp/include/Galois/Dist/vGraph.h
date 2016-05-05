/** partitioned graph wrapper -*- C++ -*-
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
 * Derived from hGraph. Graph abstraction for vertex cut.
 * @author Rashid Kaleem <rashid.kaleem@gmail.com>
 * @author Gurbinder Gill <gurbinder533@gmail.com>
 *
 */

#include <vector>
#include <set>

#include "Galois/gstl.h"
#include "Galois/Graphs/LC_CSR_Graph.h"
#include "Galois/Runtime/Substrate.h"
#include "Galois/Runtime/Network.h"

#include "Galois/Runtime/Serialize.h"

#include "Galois/Runtime/Tracer.h"

#include "Galois/Dist/GlobalObj.h"
#include "Galois/Dist/OfflineGraph.h"

#ifdef __GALOIS_HET_CUDA__
#include "Galois/Cuda/cuda_mtypes.h"
#endif

#ifndef _GALOIS_DIST_vGraph_H
#define _GALOIS_DIST_vGraph_H

/** Utilities for reading partitioned graphs. **/
struct NodeInfo {
   NodeInfo() :
         local_id(0), global_id(0), owner_id(0) {
   }
   NodeInfo(size_t l, size_t g, size_t o) :
         local_id(l), global_id(g), owner_id(o) {
   }
   size_t local_id;
   size_t global_id;
   size_t owner_id;
};

std::string getPartitionFileName(const std::string & basename, unsigned hostID, unsigned num_hosts){
   std::string result = basename;
   result+= ".PART.";
   result+=std::to_string(hostID);
   result+= ".OF.";
   result+=std::to_string(num_hosts);
   return result;
}
std::string getMetaFileName(const std::string & basename, unsigned hostID, unsigned num_hosts){
   std::string result = basename;
   result+= ".META.";
   result+=std::to_string(hostID);
   result+= ".OF.";
   result+=std::to_string(num_hosts);
   return result;
}

bool readMetaFile(const std::string& metaFileName, std::vector<NodeInfo>& localToGlobalMap_meta){
  std::ifstream meta_file(metaFileName, std::ifstream::binary);
  if (!meta_file.is_open()) {
    std::cout << "Unable to open file " << metaFileName << "! Exiting!\n";
    return false;
  }
  size_t num_entries;
  meta_file.read(reinterpret_cast<char*>(&num_entries), sizeof(num_entries));
  std::cout << "Partition :: " << " Number of nodes :: " << num_entries << "\n";
  for (size_t i = 0; i < num_entries; ++i) {
    std::pair<size_t, size_t> entry;
    size_t owner;
    meta_file.read(reinterpret_cast<char*>(&entry.first), sizeof(entry.first));
    meta_file.read(reinterpret_cast<char*>(&entry.second), sizeof(entry.second));
    meta_file.read(reinterpret_cast<char*>(&owner), sizeof(owner));
    localToGlobalMap_meta.push_back(NodeInfo(entry.second, entry.first, owner));
  }
  return true;
}


/**********Global vectors for book keeping*********************/
//std::vector<std::vector<uint64_t>> masterNodes(4); // master nodes on different hosts. For sync_pull
//std::map<uint64_t, uint32_t> GIDtoOwnerMap;

/**************************************************************/

template<typename NodeTy, typename EdgeTy, bool BSPNode=false, bool BSPEdge=false>
class vGraph : public GlobalObject {

  typedef typename std::conditional<BSPNode, std::pair<NodeTy, NodeTy>,NodeTy>::type realNodeTy;
  typedef typename std::conditional<BSPEdge, std::pair<EdgeTy, EdgeTy>,EdgeTy>::type realEdgeTy;

  typedef Galois::Graph::LC_CSR_Graph<realNodeTy, realEdgeTy> GraphTy;

  GraphTy graph;
  bool round;
  uint64_t totalNodes; // Total nodes in the complete graph.
  uint32_t numOwned; // [0, numOwned) = global nodes owned, thus [numOwned, numNodes are replicas
  uint64_t globalOffset; // [numOwned, end) + globalOffset = GID
  unsigned id; // my hostid // FIXME: isn't this just Network::ID?
  //ghost cell ID translation
  std::vector<uint64_t> ghostMap; // GID = ghostMap[LID - numOwned]
  std::vector<std::pair<uint32_t,uint32_t> > hostNodes; //LID Node owned by host i
  //pointer for each host
  std::vector<uintptr_t> hostPtrs;

  /*** Vertex Cut ***/
  std::vector<NodeInfo> localToGlobalMap_meta;
  std::vector<std::vector<size_t>> slaveNodes; // slave nodes from different hosts. For sync_push
  std::vector<std::vector<size_t>> masterNodes; // master nodes on different hosts. For sync_pull
  std::map<size_t, size_t> LocalToGlobalMap;
  std::map<size_t, size_t> GlobalToLocalMap;

  std::map<size_t, size_t> GIDtoOwnerMap;
  //GID to owner
  std::vector<std::pair<uint64_t, uint64_t>> gid2host;

  uint32_t num_recv_expected; // Number of receives expected for local completion.

#if 0
  //host -> (lid, lid]
  std::pair<uint32_t, uint32_t> nodes_by_host(uint32_t host) const {
    return hostNodes[host];
  }

  std::pair<uint64_t, uint64_t> nodes_by_host_G(uint32_t host) const {
    return gid2host[host];
  }
#endif

  size_t L2G(size_t lid) {
    //return LocalToGlobalMap[lid];
    return LocalToGlobalMap.at(lid);
  }
  size_t G2L(size_t gid) {
    //return GlobalToLocalMap[gid];
    return GlobalToLocalMap.at(gid);
  }

#if 0
  uint64_t L2G(uint32_t lid) const {
    assert(lid < graph.size());
    if (lid < numOwned)
      return lid + globalOffset;
    return ghostMap[lid - numOwned];
  }

  uint32_t G2L(uint64_t gid) const {
    if (gid >= globalOffset && gid < globalOffset + numOwned)
      return gid - globalOffset;
    auto ii = std::lower_bound(ghostMap.begin(), ghostMap.end(), gid);
    assert(*ii == gid);
    return std::distance(ghostMap.begin(), ii) + numOwned;
  }

  uint32_t L2H(uint32_t lid) const {
    assert(lid < graph.size());
    if (lid < numOwned)
      return id;
    for (int i = 0; i < hostNodes.size(); ++i)
      if (hostNodes[i].first >= lid && lid < hostNodes[i].second)
        return i;
    abort();
  }
#endif

  bool isOwned(uint64_t gid) const {
    return gid >=globalOffset && gid < globalOffset+numOwned;
  }

  template<bool en, typename std::enable_if<en>::type* = nullptr>
  NodeTy& getDataImpl(typename GraphTy::GraphNode N, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) {
    auto& r = graph.getData(N, mflag);
    return round ? r.first : r.second;
  }

  template<bool en, typename std::enable_if<!en>::type* = nullptr>
  NodeTy& getDataImpl(typename GraphTy::GraphNode N, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) {
    auto& r = graph.getData(N, mflag);
    return r;
  }

  template<bool en, typename std::enable_if<en>::type* = nullptr>
    const NodeTy& getDataImpl(typename GraphTy::GraphNode N, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) const{
      auto& r = graph.getData(N, mflag);
      return round ? r.first : r.second;
    }

    template<bool en, typename std::enable_if<!en>::type* = nullptr>
    const NodeTy& getDataImpl(typename GraphTy::GraphNode N, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) const{
      auto& r = graph.getData(N, mflag);
      return r;
    }
  template<bool en, typename std::enable_if<en>::type* = nullptr>
  typename GraphTy::edge_data_reference getEdgeDataImpl(typename GraphTy::edge_iterator ni, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) {
    auto& r = graph.getEdgeData(ni, mflag);
    return round ? r.first : r.second;
  }

  template<bool en, typename std::enable_if<!en>::type* = nullptr>
  typename GraphTy::edge_data_reference getEdgeDataImpl(typename GraphTy::edge_iterator ni, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) {
    auto& r = graph.getEdgeData(ni, mflag);
    return r;
  }

public:
  GraphTy & getGraph(){
     return graph;
  }
 

  static void syncRecv(Galois::Runtime::RecvBuffer& buf) {
    uint32_t oid;
    void (vGraph::*fn)(Galois::Runtime::RecvBuffer&);
    Galois::Runtime::gDeserialize(buf, oid, fn);
    vGraph* obj = reinterpret_cast<vGraph*>(ptrForObj(oid));
    (obj->*fn)(buf);
    //--(obj->num_recv_expected);
    //std::cout << "[ " << Galois::Runtime::getSystemNetworkInterface().ID << "] " << " NUM RECV EXPECTED : " << (obj->num_recv_expected) << "\n";
  }

  void exchange_info_landingPad(Galois::Runtime::RecvBuffer& buf){
    uint32_t hostID;
    uint64_t numItems;
    std::vector<uint64_t> items;
    Galois::Runtime::gDeserialize(buf, hostID, numItems);

    if(masterNodes.size() < hostID)
      masterNodes.resize(hostID);

    Galois::Runtime::gDeserialize(buf, masterNodes[hostID]);
    std::cout << "from : " << hostID << " -> " << numItems << " --> " << masterNodes[hostID].size() << "\n";
  }

  template<typename FnTy>
  void syncRecvApply(Galois::Runtime::RecvBuffer& buf) {
    uint32_t num;
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    Galois::Runtime::gDeserialize(buf, num);
    for(; num ; --num) {
      size_t gid;
      typename FnTy::ValTy val;
      Galois::Runtime::gDeserialize(buf, gid, val);
      //TODO: implement master
      //assert(isMaster(gid));
      auto lid = G2L(gid);
      FnTy::reduce(lid, getData(lid), val);
    }
  }

  template<typename FnTy>
  void syncPullRecvReply(Galois::Runtime::RecvBuffer& buf) {
    void (vGraph::*fn)(Galois::Runtime::RecvBuffer&) = &vGraph::syncPullRecvApply<FnTy>;
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    uint32_t num;
    unsigned from_id;
    Galois::Runtime::gDeserialize(buf, from_id, num);
    Galois::Runtime::SendBuffer b;
    assert(num == masterNodes[from_id].size());
    gSerialize(b, idForSelf(), fn, id, num);
    /*
    for(; num; --num){
      uint64_t gid;
      typename FnTy::ValTy old_val, val;
      Galois::Runtime::gDeserialize(buf, gid, old_val);
      //assert(isMaster(gid));
      val = FnTy::extract(G2L(gid), getData(G2L(gid)));

      //For now just send all.
      //if(val != old_val){
      Galois::Runtime::gSerialize(b, gid, val);
      //}
    }
    */

    for(auto n : masterNodes[from_id]){
      typename FnTy::ValTy val;
      auto localID = G2L(n);
      val = FnTy::extract((localID), getData(localID));

      Galois::Runtime::gSerialize(b, n, val);
    }
    net.send(from_id, syncRecv, b);
  }

  template<typename FnTy>
  void syncPullRecvApply(Galois::Runtime::RecvBuffer& buf) {
    assert(num_recv_expected > 0);
    uint32_t num;
    unsigned from_id;
    Galois::Runtime::gDeserialize(buf, from_id, num);
    assert(num == slaveNodes[from_id].size());
    auto& net = Galois::Runtime::getSystemNetworkInterface();

    for(; num; --num) {
      uint64_t gid;
      typename FnTy::ValTy val;
      Galois::Runtime::gDeserialize(buf, gid, val);
      auto localId = G2L(gid);
      //std::cerr << "Applying pulled val : " << val<< "\n";
      FnTy::setVal(localId, getData(localId), val);
    }
    --num_recv_expected;
  }

 public:
  typedef typename GraphTy::GraphNode GraphNode;
  typedef typename GraphTy::iterator iterator;
  typedef typename GraphTy::const_iterator const_iterator;
  typedef typename GraphTy::local_iterator local_iterator;
  typedef typename GraphTy::const_local_iterator const_local_iterator;
  typedef typename GraphTy::edge_iterator edge_iterator;


  //vGraph construction is collective
  vGraph(const std::string& filename, const std::string& partitionFolder, unsigned host, unsigned numHosts)
    :GlobalObject(this), id(host),round(false),num_recv_expected(0)
  {
    std::string part_fileName = getPartitionFileName(partitionFolder,id,numHosts);
    std::string part_metaFile = getMetaFileName(partitionFolder, id, numHosts);

    OfflineGraph g(part_fileName);
    totalNodes = g.size();
    std::cerr << "[" << id << "] SIZE ::::  " << totalNodes << "\n";
    readMetaFile(part_metaFile, localToGlobalMap_meta);
    std::cerr << "[" << id << "] MAPSIZE : " << localToGlobalMap_meta.size() << "\n";
    masterNodes.resize(numHosts);
    slaveNodes.resize(numHosts);

    for(auto info : localToGlobalMap_meta){
      assert(info.owner_id >= 0 && info.owner_id < numHosts);
      slaveNodes[info.owner_id].push_back(info.global_id);

      GIDtoOwnerMap[info.global_id] = info.owner_id;
      LocalToGlobalMap[info.local_id] = info.global_id;
      GlobalToLocalMap[info.global_id] = info.local_id;
        //Galois::Runtime::printOutput("[%] Owner : %\n", info.global_id, info.owner_id);
    }


    //Exchange information.
    exchange_info_init();

    //compute owners for all nodes
    numOwned = g.size();//gid2host[id].second - gid2host[id].first;

    uint64_t numEdges = g.edge_begin(*g.end()) - g.edge_begin(*g.begin()); // depends on Offline graph impl
    std::cerr << "[" << id << "] Edge count Done " << numEdges << "\n";


    uint32_t numNodes = numOwned;
    graph.allocateFrom(numNodes, numEdges);
    //std::cerr << "Allocate done\n";

    graph.constructNodes();
    //std::cerr << "Construct nodes done\n";
    loadEdges<std::is_void <EdgeTy>::value>(g);
  }

  template<bool isVoidType, typename std::enable_if<!isVoidType>::type* = nullptr>
  void loadEdges(OfflineGraph & g){
     fprintf(stderr, "Loading edge-data while creating edges.\n");
     uint64_t cur = 0;
     for(auto n = g.begin(); n != g.end(); ++n){
           for (auto ii = g.edge_begin(*n), ee = g.edge_end(*n); ii < ee; ++ii) {
             auto gdst = g.getEdgeDst(ii);
             auto gdata = g.getEdgeData<EdgeTy>(ii);
             graph.constructEdge(cur++, gdst, gdata);
           }
           graph.fixEndEdge((*n), cur);
         }
  }
  template<bool isVoidType, typename std::enable_if<isVoidType>::type* = nullptr>
  void loadEdges(OfflineGraph & g){
     fprintf(stderr, "Loading void edge-data while creating edges.\n");
     uint64_t cur = 0;
     for(auto n = g.begin(); n != g.end(); ++n){
           for (auto ii = g.edge_begin(*n), ee = g.edge_end(*n); ii < ee; ++ii) {
             auto gdst = g.getEdgeDst(ii);
             graph.constructEdge(cur++, gdst);
           }
           graph.fixEndEdge((*n), cur);
         }
  }

  NodeTy& getData(GraphNode N, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) {
    auto& r = getDataImpl<BSPNode>(N, mflag);
//    auto i =Galois::Runtime::NetworkInterface::ID;
    //std::cerr << i << " " << N << " " <<&r << " " << r.dist_current << "\n";
    return r;
  }

  const NodeTy& getData(GraphNode N, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) const{
    auto& r = getDataImpl<BSPNode>(N, mflag);
//    auto i =Galois::Runtime::NetworkInterface::ID;
    //std::cerr << i << " " << N << " " <<&r << " " << r.dist_current << "\n";
    return r;
  }
  typename GraphTy::edge_data_reference getEdgeData(edge_iterator ni, Galois::MethodFlag mflag = Galois::MethodFlag::WRITE) {
    return getEdgeDataImpl<BSPEdge>(ni, mflag);
  }

  GraphNode getEdgeDst(edge_iterator ni) {
    return graph.getEdgeDst(ni);
  }

  edge_iterator edge_begin(GraphNode N) {
    return graph.edge_begin(N);
  }

  edge_iterator edge_end(GraphNode N) {
    return graph.edge_end(N);
  }

  size_t size() const { return graph.size(); }
  size_t sizeEdges() const { return graph.sizeEdges(); }

  const_iterator begin() const { return graph.begin(); }
  iterator begin() { return graph.begin(); }
  const_iterator end() const { return graph.begin() + numOwned; }
  iterator end() { return graph.begin() + numOwned; }

  const_iterator ghost_begin() const { return end(); }
  iterator ghost_begin() { return end(); }
  const_iterator ghost_end() const { return graph.end(); }
  iterator ghost_end() { return graph.end(); }


  void exchange_info_init(){
    void (vGraph::*fn)(Galois::Runtime::RecvBuffer&) = &vGraph::exchange_info_landingPad;
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    for (unsigned x = 0; x < net.Num; ++x) {
      if((x == id) || (slaveNodes[x].size() == 0))
        continue;

      Galois::Runtime::SendBuffer b;
      gSerialize(b, idForSelf(), fn, id, (uint64_t)slaveNodes[x].size(), slaveNodes[x]);
      //for(auto n : slaveNodes[x]){
        //gSerialize(b, n);
      //}
      net.send(x, syncRecv, b);
      std::cout << " number of slaves from : " << x << " : " << slaveNodes[x].size() << "\n";
    }

    Galois::Runtime::getHostBarrier().wait();
  }

  template<typename FnTy>
  void sync_push() {
    void (vGraph::*fn)(Galois::Runtime::RecvBuffer&) = &vGraph::syncRecvApply<FnTy>;
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    for (unsigned x = 0; x < net.Num; ++x) {
      if((x == id) || (slaveNodes[x].size() == 0))
        continue;

      Galois::Runtime::SendBuffer b;
      gSerialize(b, idForSelf(), fn, (uint32_t)(slaveNodes[x].size()));
      for (auto start = 0; start < slaveNodes[x].size(); ++start) {
        auto gid = slaveNodes[x][start];
        auto lid = G2L(gid);

        //if(net.ID == 0)
          //std::cerr << " from 0 : GID " << gid << "\n";
        //std::cout << net.ID << " send (" << gid << ") " << start << " " << FnTy::extract(start, getData(start)) << "\n";
        gSerialize(b, gid, FnTy::extract(lid, getData(lid)));
        FnTy::reset(lid, getData(lid));
      }
      net.send(x, syncRecv, b);
    }

    //Will force all messages to be processed before continuing
    net.flush();
    Galois::Runtime::getHostBarrier().wait();

    //std::cout << "[" << net.ID <<"] time1 : " << time1.get() << "(msec) time2 : " << time2.get() << "(msec)\n";
  }


  template<typename FnTy>
  void sync_pull(){
    void (vGraph::*fn)(Galois::Runtime::RecvBuffer&) = &vGraph::syncPullRecvReply<FnTy>;
    auto& net = Galois::Runtime::getSystemNetworkInterface();

    num_recv_expected = 0;
    for (unsigned x = 0; x < net.Num; ++x) {
      if((x == id))
        continue;

      Galois::Runtime::SendBuffer b;
      gSerialize(b, idForSelf(), fn, net.ID, (uint32_t)(slaveNodes[x].size()));

     //for (auto start = 0; start < slaveNodes[x].size(); ++start) {
        //auto gid = slaveNodes[x][start];
        //auto lid = G2L(gid);
        //gSerialize(b, gid, FnTy::extract(lid, getData(lid)));
      //}
      net.send(x, syncRecv, b);
      ++num_recv_expected;
    }

    net.flush();
    while(num_recv_expected) {
      net.handleReceives();
    }

    assert(num_recv_expected == 0);
    // Can remove this barrier???.
    Galois::Runtime::getHostBarrier().wait();
  }

  uint64_t getGID(size_t nodeID) {
    return L2G(nodeID);
  }
  uint32_t getLID(uint64_t nodeID) {
    return G2L(nodeID);
  }

  unsigned getHostID(uint64_t gid){
    return GIDtoOwnerMap[gid];
  }
  uint32_t getNumOwned()const{
     return numOwned;
  }

  uint64_t getGlobalOffset()const{
     return 0;
  }
#ifdef __GALOIS_HET_CUDA__
  MarshalGraph getMarshalGraph(unsigned host_id) {
     MarshalGraph m;

     m.nnodes = size();
     m.nedges = sizeEdges();
     m.nowned = std::distance(begin(), end());
     assert(m.nowned > 0);
     m.g_offset = getGID(0);
     m.id = host_id;
     m.row_start = (index_type *) calloc(m.nnodes + 1, sizeof(index_type));
     m.edge_dst = (index_type *) calloc(m.nedges, sizeof(index_type));

     // TODO: initialize node_data and edge_data
     m.node_data = NULL;
     m.edge_data = NULL;

     // pinched from Rashid's LC_LinearArray_Graph.h

     size_t edge_counter = 0, node_counter = 0;
     for (auto n = begin(); n != ghost_end() && *n != m.nnodes; n++, node_counter++) {
        m.row_start[node_counter] = edge_counter;
        if (*n < m.nowned) {
           for (auto e = edge_begin(*n); e != edge_end(*n); e++) {
              if (getEdgeDst(e) < m.nnodes)
                 m.edge_dst[edge_counter++] = getEdgeDst(e);
           }
        }
     }

     m.row_start[node_counter] = edge_counter;
     m.nedges = edge_counter;
     return m;
  }
#endif

};
#endif//_GALOIS_DIST_vGraph_H
