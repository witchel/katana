/** Offline graph -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2014, The University of Texas at Austin. All rights reserved.
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
 * Offline graph for loading large files
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 */

#ifndef _GALOIS_DIST_OFFLINE_GRAPH_
#define _GALOIS_DIST_OFFLINE_GRAPH_

#include "Galois/Substrate/SimpleLock.h"
#include "Galois/Graphs/Details.h"

#include <cstdint>
#include <iostream>
#include <fstream>
#include <mutex>
#include <numeric>

#include <boost/iterator/counting_iterator.hpp>

namespace Galois {
namespace Graph {

//File format V1:
//version (1) {uint64_t LE}
//EdgeType size {uint64_t LE}
//numNodes {uint64_t LE}
//numEdges {uint64_t LE}
//outindexs[numNodes] {uint64_t LE} (outindex[nodeid] is index of first edge for nodeid + 1 (end interator.  node 0 has an implicit start iterator of 0.
//outedges[numEdges] {uint32_t LE}
//potential padding (32bit max) to Re-Align to 64bits
//EdgeType[numEdges] {EdgeType size}

//File format V2:
//version (2) {uint64_t LE}
//EdgeType size {uint64_t LE}
//numNodes {uint64_t LE}
//numEdges {uint64_t LE}
//outindexs[numNodes] {uint64_t LE} (outindex[nodeid] is index of first edge for nodeid + 1 (end interator.  node 0 has an implicit start iterator of 0.
//outedges[numEdges] {uint64_t LE}
//EdgeType[numEdges] {EdgeType size}


class OfflineGraph {
  std::ifstream file1;
  std::ifstream fileIndex, fileEdgeDst, fileEdgeData;
  std::streamoff loc1, locIndex, locEdgeData;

  uint64_t numNodes;
  uint64_t numEdges;
  uint64_t sizeEdgeData;
  size_t length;
  bool v2;
  uint64_t numSeeks1, numSeeksDst, numSeeksData;

  Galois::Substrate::SimpleLock lock;

  uint64_t outIndexs(uint64_t node) {
    std::lock_guard<decltype(lock)> lg(lock);
    std::streamoff pos = (4 + node)*sizeof(uint64_t);
    if (loc1 != pos){
       numSeeks1++;
      file1.seekg(pos, file1.beg);
      loc1 = pos;
    }
    uint64_t retval;
    file1.read(reinterpret_cast<char*>(&retval), sizeof(uint64_t));
    loc1 += file1.gcount();
    return retval;
  }

  uint64_t outEdges(uint64_t edge) {
    std::lock_guard<decltype(lock)> lg(lock);
    std::streamoff pos = (4 + numNodes) * sizeof(uint64_t) + edge * (v2 ? sizeof(uint64_t) : sizeof(uint32_t));
    if (locIndex != pos){
       numSeeksDst++;
       fileIndex.seekg(pos, file1.beg);
       locIndex = pos;
    }
    if (v2) {
      uint64_t retval;
      fileIndex.read(reinterpret_cast<char*>(&retval), sizeof(uint64_t));
      locIndex += fileIndex.gcount();
      return retval;
    } else {
      uint32_t retval;
      fileIndex.read(reinterpret_cast<char*>(&retval), sizeof(uint32_t));
      locIndex += fileIndex.gcount();
      return retval;
    }
  }

  template<typename T>
  T edgeData(uint64_t edge) {
    assert(sizeof(T) <= sizeEdgeData);
    std::lock_guard<decltype(lock)> lg(lock);
    std::streamoff pos = (4 + numNodes) * sizeof(uint64_t) + numEdges * (v2 ? sizeof(uint64_t) : sizeof(uint32_t));
    //align
    pos = (pos + 7) & ~7;
    pos += edge * sizeEdgeData;
    if (locEdgeData != pos){
       numSeeksData++;
       fileEdgeData.seekg(pos, file1.beg);
       locEdgeData = pos;
    }
    T retval;
    fileEdgeData.read(reinterpret_cast<char*>(&retval), sizeof(T));
    locEdgeData += fileEdgeData.gcount();
    /*fprintf(stderr, "READ:: %ld[", edge);
    for(int i=0; i<sizeof(T); ++i){
       fprintf(stderr, "%c", reinterpret_cast<char*>(&retval)[i]);
    }
    fprintf(stderr, "]");*/
    return retval;
  }

public:
  typedef boost::counting_iterator<uint32_t> iterator;
  typedef boost::counting_iterator<uint64_t> edge_iterator;
  typedef uint32_t GraphNode;

  OfflineGraph(const std::string& name)
    :file1(name, std::ios_base::binary), fileIndex(name, std::ios_base::binary),fileEdgeDst(name, std::ios_base::binary), fileEdgeData(name, std::ios_base::binary),
     loc1(0), locIndex(0), locEdgeData(0),numSeeks1(0), numSeeksDst(0), numSeeksData(0)

  {
    if (!file1.is_open() || !file1.good()) throw "Bad filename";
    uint64_t ver = 0;
    file1.read(reinterpret_cast<char*>(&ver), sizeof(uint64_t));
    file1.read(reinterpret_cast<char*>(&sizeEdgeData), sizeof(uint64_t));
    file1.read(reinterpret_cast<char*>(&numNodes), sizeof(uint64_t));
    file1.read(reinterpret_cast<char*>(&numEdges), sizeof(uint64_t));
    if (ver == 0 || ver > 2) throw "Bad Version";
    v2 = ver == 2;
    if (!file1) throw "Out of data";
    //File length
    file1.seekg(0, file1.end);
    length = file1.tellg();
    if (length < sizeof(uint64_t)*(4+numNodes) + (v2 ? sizeof(uint64_t) : sizeof(uint32_t))*numEdges)
      throw "File too small";
    
    file1.seekg(0, std::ios_base::beg);
    fileEdgeDst.seekg(0, std::ios_base::beg);
    fileEdgeData.seekg(0, std::ios_base::beg);
    fileIndex.seekg(0, std::ios_base::beg);
  }
  uint64_t num_seeks(){
     std::cout << "Seeks :: " << numSeeks1 << " , " << numSeeksData << " , " << numSeeksDst << " \n";
     return numSeeks1+numSeeksData+numSeeksDst;
  }
  void reset_seek_counters(){
     numSeeks1=numSeeksData=numSeeksDst=0;
  }

  OfflineGraph(OfflineGraph&&) = default;


  size_t size() const { return numNodes; }
  size_t sizeEdges() const { return numEdges; }
  size_t edgeSize() const { return sizeEdgeData; }

  iterator begin() { return iterator(0); }
  iterator end() { return iterator(numNodes); }

  edge_iterator edge_begin(GraphNode N) {
    if (N == 0)
      return edge_iterator(0);
    else
      return edge_iterator(outIndexs(N-1));
  }
  
  edge_iterator edge_end(GraphNode N) {
    return edge_iterator(outIndexs(N));
  }

  GraphNode getEdgeDst(edge_iterator ni) {
    return outEdges(*ni);
  }

  Runtime::iterable<NoDerefIterator<edge_iterator>> edges(GraphNode N) {
    return detail::make_no_deref_range(edge_begin(N), edge_end(N));
  }


  template<typename T>
  T getEdgeData(edge_iterator ni) {
    return edgeData<T>(*ni);
  }

};


class OfflineGraphWriter {
  std::fstream file;
  uint64_t numNodes, numEdges;
  bool smallData;

  std::deque<uint64_t> edgeOffsets;

  std::streamoff offsetOfDst(uint64_t edge) {
    return sizeof(uint64_t)*(4 + numNodes + edge);
  }
  std::streamoff offsetOfData(uint64_t edge) {
    return sizeof(uint64_t) * (4 + numNodes + numEdges) + 
      (smallData ? sizeof(float) : sizeof(double)) * edge;
  }

  void setEdge32(uint64_t src, uint64_t offset, uint64_t dst, uint32_t val) {
    if (src)
      offset += edgeOffsets[src-1];
    file.seekg(offsetOfDst(offset), std::ios_base::beg);
    file.write(reinterpret_cast<char*>(&dst), sizeof(uint64_t));
    file.seekg(offsetOfData(offset), std::ios_base::beg);
    file.write(reinterpret_cast<char*>(&val), sizeof(uint32_t));
  }

  void setEdge64(uint64_t src, uint64_t offset, uint64_t dst, uint64_t val) {
    if (src)
      offset += edgeOffsets[src-1];
    file.seekg(offsetOfDst(offset), std::ios_base::beg);
    file.write(reinterpret_cast<char*>(&dst), sizeof(uint64_t));
    file.seekg(offsetOfData(offset), std::ios_base::beg);
    file.write(reinterpret_cast<char*>(&val), sizeof(uint64_t));
  }


public:
OfflineGraphWriter(const std::string& name, bool use32=false) :file(name, std::ios_base::in | std::ios_base::out | std::ios_base::binary | std::ios_base::trunc), numNodes(0), numEdges(0), smallData(use32) {
    if (!file.is_open() || !file.good()) throw "Bad filename";
    uint64_t ver = 1;
    uint64_t etSize = smallData ? sizeof(float) : sizeof(double);
    file.write(reinterpret_cast<char*>(&ver), sizeof(uint64_t));
    file.write(reinterpret_cast<char*>(&etSize), sizeof(uint64_t));
    file.write(reinterpret_cast<char*>(&numNodes), sizeof(uint64_t));
    file.write(reinterpret_cast<char*>(&numEdges), sizeof(uint64_t));
    file.seekg(0, std::ios_base::beg);
  }

  ~OfflineGraphWriter() {}

  //sets the number of nodes and edges.  points to an container of edge counts
  void setCounts(std::deque<uint64_t> edgeCounts) {
    edgeOffsets = std::move(edgeCounts);
    numNodes = edgeOffsets.size();
    numEdges = std::accumulate(edgeOffsets.begin(),edgeOffsets.end(),0);
    std::partial_sum(edgeOffsets.begin(), edgeOffsets.end(), edgeOffsets.begin());
    file.seekg(sizeof(uint64_t)*2, std::ios_base::beg);
    file.write(reinterpret_cast<char*>(&numNodes), sizeof(uint64_t));
    file.write(reinterpret_cast<char*>(&numEdges), sizeof(uint64_t));
    for (auto i : edgeOffsets)
      file.write(reinterpret_cast<char*>(&i), sizeof(uint64_t));
    file.seekg(0, std::ios_base::beg);
  }

  void setEdge(uint64_t src, uint64_t offset, uint64_t dst, uint64_t val) {
    if(smallData)
      setEdge32(src,offset,dst,val);
    else
      setEdge64(src,offset,dst,val);
  }
};


} // namespace Graph
} // namespace Galois

#endif//_GALOIS_DIST_OFFLINE_GRAPH_