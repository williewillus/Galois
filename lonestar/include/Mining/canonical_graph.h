/*
 * canonical_graph.hpp
 *  Created on: Aug 4, 2017
 *      Author: icuzzq
 */
#ifndef CORE_CANONICAL_GRAPH_HPP_
#define CORE_CANONICAL_GRAPH_HPP_
#include "type.h"

class CanonicalGraph {
	friend std::ostream & operator<<(std::ostream & strm, const CanonicalGraph& cg);
public:
	CanonicalGraph() : number_of_vertices(0), hash_value(0) {}
	CanonicalGraph(bliss::AbstractGraph* ag, bool is_directed) {
		construct_cg(ag, is_directed);
	}
	~CanonicalGraph() {}
	int cmp(const CanonicalGraph& other_cg) const {
		//compare the numbers of vertices
		if(get_number_vertices() < other_cg.get_number_vertices()) return -1;
		if(get_number_vertices() > other_cg.get_number_vertices()) return 1;
		//compare hash value
		if(get_hash() < other_cg.get_hash()) return -1;
		if(get_hash() > other_cg.get_hash()) return 1;
		//compare edges
		assert(embedding.size() == other_cg.embedding.size());
		for(unsigned i = 0; i < embedding.size(); ++i) {
			const auto & t1 = embedding[i];
			const auto & t2 = other_cg.embedding[i];
			int cmp_element = t1.cmp(t2);
			if(cmp_element != 0) return cmp_element;
		}
		return 0;
	}
	inline unsigned int get_hash() const { return hash_value; }
	inline int get_number_vertices() const { return number_of_vertices; }
	//operator for map
	inline bool operator==(const CanonicalGraph& other) const { return cmp(other) == 0; }
	inline Embedding& get_embedding() { return embedding; }
	inline Embedding get_embedding_const() const { return embedding; }
	inline void set_number_vertices(int num_vertices) { number_of_vertices = num_vertices; }
	inline void set_hash_value(unsigned int hash) { hash_value = hash; }
	inline unsigned get_quick_pattern_index(unsigned i) { return qp_idx[i]; }
private:
	Embedding embedding;
	std::vector<int> qp_idx;
	int number_of_vertices;
	unsigned int hash_value;
	void construct_cg(bliss::AbstractGraph* ag, bool is_directed) {
		assert(!is_directed);
		if(!is_directed) {
			number_of_vertices = ag->get_nof_vertices();
			hash_value = ag->get_hash();
			//std::cout << number_of_vertices << ", " << hash_value << std::endl;
			transform_to_embedding(ag);
		}
	}
	void transform_to_embedding(bliss::AbstractGraph* ag) {
		bliss::Graph* graph = (bliss::Graph*) ag;
		std::unordered_set<VertexId> set;
		std::unordered_map<VertexId, BYTE> map;
		std::priority_queue<Edge, std::vector<Edge>, EdgeComparator> min_heap;
		std::vector<bliss::Graph::Vertex> vertices = graph->get_vertices_rstream();
		//std::cout << "number of vertices: " << vertices.size() << std::endl;
		VertexId first_src = init_heapAndset(vertices, min_heap, set);
		assert(first_src != -1);
		push_first_element(first_src, map, vertices);
#ifdef USE_DOMAIN
		bool is_first_edge = true;
#endif
		while(!min_heap.empty()) {
			Edge edge = min_heap.top();
			//std::cout << "embedding: " << embedding << std::endl;
#ifdef USE_DOMAIN
			if (is_first_edge) {
				qp_idx.push_back(edge.src_domain);
				is_first_edge = false;
			}
#endif
			push_element(edge, map, vertices);
			min_heap.pop();
			add_neighbours(edge, min_heap, vertices, set);
		}
	}
	VertexId init_heapAndset(std::vector<bliss::Graph::Vertex>& vertices, std::priority_queue<Edge, std::vector<Edge>, EdgeComparator>& min_heap, std::unordered_set<VertexId>& set) {
		for(unsigned i = 0; i < vertices.size(); ++i) {
			if(!vertices[i].edges.empty()) {
				for(auto v: vertices[i].edges) {
#ifdef USE_DOMAIN
					min_heap.push(Edge(i, v.first, v.second.first, v.second.second));
#else
					min_heap.push(Edge(i, v));
#endif
				}
				set.insert(i);
				return i;
			}
		}
		return -1;
	}
	void push_first_element(VertexId first, std::unordered_map<VertexId, BYTE>& map, std::vector<bliss::Graph::Vertex>& vertices){
		map[first] = 0;
		embedding.push_back(ElementType(first + 1, (BYTE)0, (BYTE)vertices[first].color, (BYTE)0));
	}
	void push_element(Edge& edge, std::unordered_map<VertexId, BYTE>& map, std::vector<bliss::Graph::Vertex>& vertices){
		assert(edge.src < edge.target);
		if(map.find(edge.src) != map.end()) {
			embedding.push_back(ElementType(edge.target + 1, (BYTE)0, (BYTE)vertices[edge.target].color, (BYTE)map[edge.src]));
#ifdef USE_DOMAIN
			qp_idx.push_back(edge.target_domain);
#endif
			if(map.find(edge.target) == map.end()) {
				int s = embedding.size() - 1;
				map[edge.target] = s;
			}
		} else if(map.find(edge.target) != map.end()) {
			embedding.push_back(ElementType(edge.src + 1, (BYTE)0, (BYTE)vertices[edge.src].color, (BYTE)map[edge.target]));
#ifdef USE_DOMAIN
			qp_idx.push_back(edge.src_domain);
#endif
			if(map.find(edge.src) == map.end()) {
				int s = embedding.size() - 1;
				map[edge.src] = s;
			}
		} else {
			//wrong case
			std::cout << "wrong case!!!" << std::endl;
			throw std::exception();
		}
	}
	void add_neighbours(Edge& edge, std::priority_queue<Edge, std::vector<Edge>, EdgeComparator>& min_heap, std::vector<bliss::Graph::Vertex>& vertices, std::unordered_set<VertexId>& set) {
		add_neighbours(edge.src, min_heap, vertices, set);
		add_neighbours(edge.target, min_heap, vertices, set);
	}

	void add_neighbours(VertexId srcId, std::priority_queue<Edge, std::vector<Edge>, EdgeComparator>& min_heap, std::vector<bliss::Graph::Vertex>& vertices, std::unordered_set<VertexId>& set) {
		if(set.find(srcId) == set.end()){
			for(auto v: vertices[srcId].edges) {
#ifdef USE_DOMAIN
				VertexId target = v.first;
#else
				VertexId target = v;
#endif
				if(set.find(target) == set.end()) {
#ifdef USE_DOMAIN
					Edge edge(srcId, target, v.second.first, v.second.second);
#else
					Edge edge(srcId, target);
#endif
					edge.swap();
					min_heap.push(edge);
				}
			}
			set.insert(srcId);
		}
	}
};

std::ostream & operator<<(std::ostream & strm, const CanonicalGraph& cg) {
	strm << "{" << cg.get_embedding_const() << "; " << cg.get_number_vertices() << "; " << cg.get_hash() << "}";
	return strm;
}

namespace std {
template<>
struct hash<CanonicalGraph> {
	std::size_t operator()(const CanonicalGraph& cg) const {
		//simple hash
		return std::hash<int>()(cg.get_hash());
	}
};
}
#endif /* CORE_CANONICAL_GRAPH_HPP_ */
