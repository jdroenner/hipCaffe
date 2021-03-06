#ifndef CPU_ONLY
#include <hip/hip_runtime.h>
#endif
#include <glog/logging.h>
#include <stdio.h>

#include <sstream>
#include <string>
#include <vector>

#include "boost/thread.hpp"
#include "caffe/caffe.hpp"
#include "caffe/parallel.hpp"

namespace caffe {

enum Op {
  copy,
  replace_cpu,
  replace_gpu,
  replace_cpu_diff,
  replace_gpu_diff
};

template<typename Dtype>
static void apply_buffers(const vector<Blob<Dtype>*>& blobs,
                          Dtype* buffer, size_t total_size, Op op) {
  Dtype* ptr = buffer;
  for (int i = 0; i < blobs.size(); ++i) {
    int size = blobs[i]->count();
    switch (op) {
      case copy: {
        // Init buffer to current values of blobs
        caffe_copy(size,
                   reinterpret_cast<const Dtype*>(blobs[i]->data()->cpu_data()),
                   ptr);
        break;
      }
      case replace_cpu:
        blobs[i]->data()->set_cpu_data(ptr);
        break;
      case replace_gpu:
        blobs[i]->data()->set_gpu_data(ptr);
        break;
      case replace_cpu_diff:
        blobs[i]->diff()->set_cpu_data(ptr);
        break;
      case replace_gpu_diff:
        blobs[i]->diff()->set_gpu_data(ptr);
        break;
    }
    ptr += size;
  }
  // total_size is at least one byte
  CHECK_EQ(total_size, (ptr == buffer ? 1 : ptr - buffer));
}

// Buffer size necessary to store given blobs
template<typename Dtype>
static size_t total_size(const vector<Blob<Dtype>*>& params) {
  size_t size = 0;
  for (int i = 0; i < params.size(); ++i)
    size += params[i]->count();
  // Size have at least one byte, otherwise hipMalloc fails if net has no
  // learnable parameters.
  return (size > 0) ? size : 1;
}

template<typename Dtype>
Params<Dtype>::Params(shared_ptr<Solver<Dtype> > root_solver)
    : size_(total_size<Dtype>(root_solver->net()->learnable_params())),
      data_(),
      diff_() {
}

template<typename Dtype>
GPUParams<Dtype>::GPUParams(shared_ptr<Solver<Dtype> > root_solver, int device)
    : Params<Dtype>(root_solver) {
#ifndef CPU_ONLY
  int initial_device;
  HIP_CHECK(hipGetDevice(&initial_device));

  // Allocate device buffers
  HIP_CHECK(hipSetDevice(device));
  HIP_CHECK(hipMalloc(&data_, size_ * sizeof(Dtype)));

  // Copy blob values
  const vector<Blob<Dtype>*>& net =
      root_solver->net()->learnable_params();
  apply_buffers(net, data_, size_, copy);

  HIP_CHECK(hipMalloc(&diff_, size_ * sizeof(Dtype)));
  caffe_gpu_set(size_, Dtype(0), diff_);

  HIP_CHECK(hipSetDevice(initial_device));
#else
  NO_GPU;
#endif
}

template<typename Dtype>
GPUParams<Dtype>::~GPUParams() {
#ifndef CPU_ONLY
  HIP_CHECK(hipFree(data_));
  HIP_CHECK(hipFree(diff_));
#endif
}

template<typename Dtype>
void GPUParams<Dtype>::configure(Solver<Dtype>* solver) const {
  const vector<Blob<Dtype>*>& net =
      solver->net()->learnable_params();
  apply_buffers(net, data_, size_, replace_gpu);
  apply_buffers(net, diff_, size_, replace_gpu_diff);
}

void DevicePair::compute(const vector<int> devices, vector<DevicePair>* pairs) {
#ifndef CPU_ONLY
  vector<int> remaining(devices);

  // Depth for reduction tree
  int remaining_depth = static_cast<int>(ceil(log2((float)remaining.size())));

  // Group GPUs by board
  for (int d = 0; d < remaining_depth; ++d) {
    for (int i = 0; i < remaining.size(); ++i) {
      for (int j = i + 1; j < remaining.size(); ++j) {
        hipDeviceProp_t a, b;
        HIP_CHECK(hipGetDeviceProperties(&a, remaining[i]));
        HIP_CHECK(hipGetDeviceProperties(&b, remaining[j]));
        // TODO: HIP Equivalent
        /*if (a.isMultiGpuBoard && b.isMultiGpuBoard) {
          if (a.multiGpuBoardGroupID == b.multiGpuBoardGroupID) {
            pairs->push_back(DevicePair(remaining[i], remaining[j]));
            DLOG(INFO) << "GPU board: " << remaining[i] << ":" << remaining[j];
            remaining.erase(remaining.begin() + j);
            break;
          }
        }*/
      }
    }
  }
  ostringstream s;
  for (int i = 0; i < remaining.size(); ++i) {
    s << (i ? ", " : "") << remaining[i];
  }
  DLOG(INFO) << "GPUs paired by boards, remaining: " << s.str();

  // Group by P2P accessibility
  remaining_depth = ceil(log2((float)remaining.size()));
  for (int d = 0; d < remaining_depth; ++d) {
    for (int i = 0; i < remaining.size(); ++i) {
      for (int j = i + 1; j < remaining.size(); ++j) {
        int access;
        HIP_CHECK(
            hipDeviceCanAccessPeer(&access, remaining[i], remaining[j]));
        if (access) {
          pairs->push_back(DevicePair(remaining[i], remaining[j]));
          DLOG(INFO) << "P2P pair: " << remaining[i] << ":" << remaining[j];
          remaining.erase(remaining.begin() + j);
          break;
        }
      }
    }
  }
  s.str("");
  for (int i = 0; i < remaining.size(); ++i) {
    s << (i ? ", " : "") << remaining[i];
  }
  DLOG(INFO) << "GPUs paired by P2P access, remaining: " << s.str();

  // Group remaining
  remaining_depth = ceil(log2((float)remaining.size()));
  for (int d = 0; d < remaining_depth; ++d) {
    for (int i = 0; i < remaining.size(); ++i) {
      pairs->push_back(DevicePair(remaining[i], remaining[i + 1]));
      DLOG(INFO) << "Remaining pair: " << remaining[i] << ":"
                 << remaining[i + 1];
      remaining.erase(remaining.begin() + i + 1);
    }
  }

  // Should only be the parent node remaining
  CHECK_EQ(remaining.size(), 1);

  pairs->insert(pairs->begin(), DevicePair(-1, remaining[0]));

  CHECK(pairs->size() == devices.size());
  for (int i = 0; i < pairs->size(); ++i) {
    CHECK((*pairs)[i].parent() != (*pairs)[i].device());
    for (int j = i + 1; j < pairs->size(); ++j) {
      CHECK((*pairs)[i].device() != (*pairs)[j].device());
    }
  }
#else
  NO_GPU;
#endif
}

//

template<typename Dtype>
P2PSync<Dtype>::P2PSync(shared_ptr<Solver<Dtype> > root_solver,
                        P2PSync<Dtype>* parent, const SolverParameter& param)
    : GPUParams<Dtype>(root_solver, param.device_id()),
      parent_(parent),
      children_(),
      queue_(),
      initial_iter_(root_solver->iter()),
      solver_() {
#ifndef CPU_ONLY
  int initial_device;
  HIP_CHECK(hipGetDevice(&initial_device));
  this->device_ = param.device_id();
  HIP_CHECK(hipSetDevice(device_));

  if (parent == NULL) {
    solver_ = root_solver;
  } else {
    Caffe::set_root_solver(false);
    solver_.reset(new WorkerSolver<Dtype>(param, root_solver.get()));
    Caffe::set_root_solver(true);
  }
  this->configure(solver_.get());
  solver_->add_callback(this);

  if (parent) {
    // Enable p2p access between devices
    const int peer = parent->solver_->param().device_id();
    int access;
    HIP_CHECK(hipDeviceCanAccessPeer(&access, device_, peer));
    if (access) {
      HIP_CHECK(hipDeviceEnablePeerAccess(peer, 0));
    } else {
      LOG(INFO)<< "GPU " << device_ << " does not have p2p access to GPU " << peer;
    }
    // Allocate receiving buffer on parent
    HIP_CHECK(hipSetDevice(peer));
    HIP_CHECK(hipMalloc(&parent_grads_, size_ * sizeof(Dtype)));
    HIP_CHECK(hipSetDevice(device_));
  }

  HIP_CHECK(hipSetDevice(initial_device));
#else
  NO_GPU;
#endif
}

template<typename Dtype>
P2PSync<Dtype>::~P2PSync() {
#ifndef CPU_ONLY
  int initial_device;
  HIP_CHECK(hipGetDevice(&initial_device));
  const int self = solver_->param().device_id();
  HIP_CHECK(hipSetDevice(self));

  if (parent_) {
    HIP_CHECK(hipFree(parent_grads_));
    const int peer = parent_->solver_->param().device_id();
    int access;
    HIP_CHECK(hipDeviceCanAccessPeer(&access, self, peer));
    if (access) {
      HIP_CHECK(hipDeviceDisablePeerAccess(peer));
    }
  }

  HIP_CHECK(hipSetDevice(initial_device));
#endif
}

template<typename Dtype>
void P2PSync<Dtype>::InternalThreadEntry() {
  Caffe::SetDevice(solver_->param().device_id());
  CHECK(Caffe::root_solver());
  Caffe::set_root_solver(false);
  // See if there is a defined seed and reset random state if so
  if (solver_->param().random_seed() >= 0) {
    // Fetch random seed and modulate by device ID to make sure
    // everyone doesn't have the same seed.  We seem to have some
    // solver instability if we have everyone with the same seed
    Caffe::set_random_seed(
        solver_->param().random_seed() + solver_->param().device_id());
  }
  solver_->Step(solver_->param().max_iter() - initial_iter_);
}

template<typename Dtype>
void P2PSync<Dtype>::on_start() {
#ifndef CPU_ONLY
#ifdef DEBUG
  int device;
  HIP_CHECK(hipGetDevice(&device));
  CHECK(device == solver_->param().device_id());
#else
//  CHECK(false);
#endif

  // Wait for update from parent
  if (parent_) {
    P2PSync<Dtype> *parent = queue_.pop();
    CHECK(parent == parent_);
  }

  // Update children
  for (int i = children_.size() - 1; i >= 0; i--) {
    Dtype* src = data_;
    Dtype* dst = children_[i]->data_;

#ifdef DEBUG
    hipPointerAttribute_t attributes;
    HIP_CHECK(hipPointerGetAttributes(&attributes, src));
    CHECK(attributes.device == device);
    HIP_CHECK(hipPointerGetAttributes(&attributes, dst));
    CHECK(attributes.device == children_[i]->solver_->param().device_id());
#endif

    HIP_CHECK(hipMemcpyAsync(dst, src, size_ * sizeof(Dtype),
        hipMemcpyDeviceToDevice, hipStreamDefault));
    HIP_CHECK(hipStreamSynchronize(hipStreamDefault));
    children_[i]->queue_.push(this);
  }
#endif
}

template<typename Dtype>
void P2PSync<Dtype>::on_gradients_ready() {
#ifndef CPU_ONLY
  HIP_SCOPED_MARKER(__func__, "Parallel");
#ifdef DEBUG
  int device;
  HIP_CHECK(hipGetDevice(&device));
  CHECK(device == solver_->param().device_id());
#endif

  // Sum children gradients as they appear in the queue
  for (int i = 0; i < children_.size(); ++i) {
    P2PSync<Dtype> *child = queue_.pop();
    Dtype* src = child->parent_grads_;
    Dtype* dst = diff_;

#ifdef DEBUG
    bool ok = false;
    for (int j = 0; j < children_.size(); ++j) {
      if (child == children_[j]) {
        ok = true;
      }
    }
    CHECK(ok);
    hipPointerAttribute_t attributes;
    HIP_CHECK(hipPointerGetAttributes(&attributes, src));
    CHECK(attributes.device == device);
    HIP_CHECK(hipPointerGetAttributes(&attributes, dst));
    CHECK(attributes.device == device);
#endif

    caffe_gpu_add(size_, src, dst, dst);
  }

  // Send gradients to parent
  if (parent_) {
    Dtype* src = diff_;
    Dtype* dst = parent_grads_;

#ifdef DEBUG
    hipPointerAttribute_t attributes;
    HIP_CHECK(hipPointerGetAttributes(&attributes, src));
    CHECK(attributes.device == device);
    HIP_CHECK(hipPointerGetAttributes(&attributes, dst));
    CHECK(attributes.device == parent_->solver_->param().device_id());
#endif

    HIP_CHECK(hipMemcpyAsync(dst, src, size_ * sizeof(Dtype),  //
        hipMemcpyDeviceToDevice, hipStreamDefault));
    HIP_CHECK(hipStreamSynchronize(hipStreamDefault));
    parent_->queue_.push(this);
  } else {
    // Loss functions divide gradients by the batch size, so to compensate
    // for split batch, the root solver divides by number of solvers.
    caffe_gpu_scal(size_, Dtype(1.0 / Caffe::solver_count()), diff_);
  }
#endif
}

template<typename Dtype>
void P2PSync<Dtype>::Prepare(const vector<int>& gpus,
            vector<shared_ptr<P2PSync<Dtype> > >* syncs) {
  // Pair devices for map-reduce synchronization
  vector<DevicePair> pairs;
  DevicePair::compute(gpus, &pairs);
  ostringstream s;
  for (int i = 1; i < pairs.size(); ++i) {
    s << (i == 1 ? "" : ", ") << pairs[i].parent() << ":" << pairs[i].device();
  }
  LOG(INFO)<< "GPUs pairs " << s.str();

  SolverParameter param(solver_->param());

  // Build the GPU tree by finding the parent for each solver
  for (int attempts = 0; attempts < pairs.size(); ++attempts) {
    for (int i = 1; i < pairs.size(); ++i) {
      if (!syncs->at(i).get()) {
        P2PSync<Dtype>* parent = NULL;
        for (int j = 0; j < syncs->size(); ++j) {
          P2PSync<Dtype>* sync = j == 0 ? this : syncs->at(j).get();
          if (sync) {
            const SolverParameter& p = sync->solver()->param();
            if (p.device_id() == pairs[i].parent()) {
              parent = sync;
            }
          }
        }
        if (parent) {
          param.set_device_id(pairs[i].device());
          syncs->at(i).reset(new P2PSync<Dtype>(solver_, parent, param));
          parent->children_.push_back((P2PSync<Dtype>*) syncs->at(i).get());
        }
      }
    }
  }
}

template<typename Dtype>
void P2PSync<Dtype>::Run(const vector<int>& gpus) {
  vector<shared_ptr<P2PSync<Dtype> > > syncs(gpus.size());
  Prepare(gpus, &syncs);

  LOG(INFO)<< "Starting Optimization";

  DLOG(INFO) << "Start " << (syncs.size() - 1) << " threads";
  for (int i = 1; i < syncs.size(); ++i) {
    syncs[i]->StartInternalThread();
  }

  DLOG(INFO) << "Run root solver";

  // Run root solver on current thread
  solver_->Solve();

  DLOG(INFO) << "Stop " << (syncs.size() - 1) << " threads";
  for (int i = 1; i < syncs.size(); ++i) {
    syncs[i]->StopInternalThread();
  }
}

INSTANTIATE_CLASS(Params);
INSTANTIATE_CLASS(GPUParams);
INSTANTIATE_CLASS(P2PSync);

}  // namespace caffe
