#pragma once
#include "Galois/Cuda/cuda_mtypes.h"

struct CUDA_Context;

struct CUDA_Context *get_CUDA_context(int id);
bool init_CUDA_context(struct CUDA_Context *ctx, int device);
void load_graph_CUDA(struct CUDA_Context *ctx, MarshalGraph &g);

void reset_CUDA_context(struct CUDA_Context *ctx);
int get_node_nout_cuda(struct CUDA_Context *ctx, unsigned LID);
void set_node_nout_cuda(struct CUDA_Context *ctx, unsigned LID, int v);
void add_node_nout_cuda(struct CUDA_Context *ctx, unsigned LID, int v);
float get_node_value_cuda(struct CUDA_Context *ctx, unsigned LID);
void set_node_value_cuda(struct CUDA_Context *ctx, unsigned LID, float v);
void add_node_value_cuda(struct CUDA_Context *ctx, unsigned LID, float v);
void InitializeGraph_cuda(const float & local_alpha, struct CUDA_Context *ctx);
void PageRank_pull_cuda(int & __retval, const float & local_alpha, const float & local_tolerance, struct CUDA_Context *ctx);
