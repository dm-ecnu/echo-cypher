//
// Created by ssunah on 6/22/18.
//

#ifndef SUBGRAPHMATCHING_GRAPH_H
#define SUBGRAPHMATCHING_GRAPH_H

#include <unordered_map>
#include <vector>
#include <algorithm>
#include <iostream>

#include "configuration/types.h"
#include "configuration/config.h"

/**
 * A graph is stored as the CSR format.
 */

class Graph {
private:
    bool enable_label_offset_;

    ui vertices_count_;
    ui edges_count_;
    ui labels_count_;
    ui max_degree_;
    ui max_label_frequency_;

    ui* offsets_;
    VertexID * neighbors_;
    LabelID* labels_;
    ui* reverse_index_offsets_;
    ui* reverse_index_;

    int* core_table_;
    ui core_length_;

    std::unordered_map<LabelID, ui> labels_frequency_;

#if OPTIMIZED_LABELED_GRAPH == 1
    ui* labels_offsets_;
    std::unordered_map<LabelID, ui>* nlf_;
#endif

private:
    void BuildReverseIndex();

#if OPTIMIZED_LABELED_GRAPH == 1
    void BuildNLF();
    void BuildLabelOffset();
#endif

public:
    // Optional input-domain adapter. Legacy Graph objects retain their layout.
    virtual bool echoHasDomains() const { return false; }
    virtual bool echoPermits(ui, VertexID) const { return true; }
    Graph(const bool enable_label_offset) {
        enable_label_offset_ = enable_label_offset;

        vertices_count_ = 0;
        edges_count_ = 0;
        labels_count_ = 0;
        max_degree_ = 0;
        max_label_frequency_ = 0;
        core_length_ = 0;

        offsets_ = NULL;
        neighbors_ = NULL;
        labels_ = NULL;
        reverse_index_offsets_ = NULL;
        reverse_index_ = NULL;
        core_table_ = NULL;
        labels_frequency_.clear();

#if OPTIMIZED_LABELED_GRAPH == 1
        labels_offsets_ = NULL;
        nlf_ = NULL;
#endif
    }

    virtual ~Graph() {
        delete[] offsets_;
        delete[] neighbors_;
        delete[] labels_;
        delete[] reverse_index_offsets_;
        delete[] reverse_index_;
        delete[] core_table_;
#if OPTIMIZED_LABELED_GRAPH == 1
        delete[] labels_offsets_;
        delete[] nlf_;
#endif
    }

public:
    virtual void loadGraphFromFile(const std::string& file_path);
    virtual void loadGraphFromFileCompressed(const std::string& degree_path, const std::string& edge_path,
                                     const std::string& label_path);
    void storeComparessedGraph(const std::string& degree_path, const std::string& edge_path,
                               const std::string& label_path);
    void printGraphMetaData();
public:
    virtual const ui getLabelsCount() const {
        return labels_count_;
    }

    virtual const ui getVerticesCount() const {
        return vertices_count_;
    }

    virtual const ui getEdgesCount() const {
        return edges_count_;
    }

    virtual const ui getGraphMaxDegree() const {
        return max_degree_;
    }

    virtual const ui getGraphMaxLabelFrequency() const {
        return max_label_frequency_;
    }

    virtual const ui getVertexDegree(const VertexID id) const {
        return offsets_[id + 1] - offsets_[id];
    }

    virtual const ui getLabelsFrequency(const LabelID label) const {
        return labels_frequency_.find(label) == labels_frequency_.end() ? 0 : labels_frequency_.at(label);
    }

    virtual const ui getCoreValue(const VertexID id) const {
        return core_table_[id];
    }

    virtual const ui get2CoreSize() const {
        return core_length_;
    }
    virtual const LabelID getVertexLabel(const VertexID id) const {
        return labels_[id];
    }

    // ECHO N4 hook: the envelope pattern labels each class with the *set* of its members'
    // labels. The filter's label check asks whether a data vertex can stand for a class, not
    // whether its single stored label equals one id, so it goes through this predicate. The base
    // graph keeps the ordinary equality; EnvDataView (src/echo_mask.cpp) overrides it.
    virtual bool hasVertexLabel(const VertexID id, const LabelID label) const {
        return getVertexLabel(id) == label;
    }

    // True only for the envelope data view, whose classes stand for label sets. The base graph
    // keeps the direct label comparison on the hot filter path (no per-neighbour virtual call).
    virtual bool hasLabelSets() const { return false; }

    virtual const ui * getVertexNeighbors(const VertexID id, ui& count) const {
        count = offsets_[id + 1] - offsets_[id];
        return neighbors_ + offsets_[id];
    }


    virtual const ui * getVerticesByLabel(const LabelID id, ui& count) const {
        count = reverse_index_offsets_[id + 1] - reverse_index_offsets_[id];
        return reverse_index_ + reverse_index_offsets_[id];
    }

#if OPTIMIZED_LABELED_GRAPH == 1
    virtual const ui * getNeighborsByLabel(const VertexID id, const LabelID label, ui& count) const {
        ui offset = id * labels_count_ + label;
        count = labels_offsets_[offset + 1] - labels_offsets_[offset];
        return neighbors_ + labels_offsets_[offset];
    }

    virtual const std::unordered_map<LabelID, ui>* getVertexNLF(const VertexID id) const {
        return nlf_ + id;
    }

    virtual bool checkEdgeExistence(const VertexID u, const VertexID v, const LabelID u_label) const {
        ui count = 0;
        const VertexID* neighbors = getNeighborsByLabel(v, u_label, count);
        int begin = 0;
        int end = count - 1;
        while (begin <= end) {
            int mid = begin + ((end - begin) >> 1);
            if (neighbors[mid] == u) {
                return true;
            }
            else if (neighbors[mid] > u)
                end = mid - 1;
            else
                begin = mid + 1;
        }

        return false;
    }
#endif

    bool checkEdgeExistence(VertexID u, VertexID v) const {
        if (getVertexDegree(u) < getVertexDegree(v)) {
            std::swap(u, v);
        }
        ui count = 0;
        const VertexID* neighbors =  getVertexNeighbors(v, count);

        int begin = 0;
        int end = count - 1;
        while (begin <= end) {
            int mid = begin + ((end - begin) >> 1);
            if (neighbors[mid] == u) {
                return true;
            }
            else if (neighbors[mid] > u)
                end = mid - 1;
            else
                begin = mid + 1;
        }

        return false;
    }

    void buildCoreTable();
};


#endif //SUBGRAPHMATCHING_GRAPH_H
