#include <iostream>

#include <mpi.h>
#include <pybind11/pybind11.h>

#include <bind/PybindUtil.h>
#include <comm/NCCLWrapper.h>
#include <comm/ObjectComm.h>
#include <comm/SComm.h>
#include <comp/Backward.h>
#include <comp/EventRecorder.h>
#include <graph/DPStaging.h>

#include "bind/RaNNCFactory.h"
#include "bind/RaNNCProcess.h"
#include "bind/Tracer.h"
#include "comm/MPIUtil.h"
#include "comp/DistributedParamLocator.h"
#include "comp/RaNNCModule.h"
#include "cuda/CudaSync.h"
#include "graph/DeploymentSerializer.h"
#include "Logging.h"

#include "cpg/CPG.h"
#include "distop/DistMatmul.h"

namespace py = pybind11;
using namespace rannc;

PYBIND11_MODULE(_pyrannc, m) {
  m.def("test_cpg", []() { testCPG(); });

  m.add_object("_cleanup", py::capsule([]() {
                 EventRecorder& erec = EventRecorder::get();
                 erec.dump(config::Config::get().getVal<std::string>(
                     config::EVENT_TRACE_FILE));

                 auto process = RaNNCFactory::get();
                 for (auto& it : process->getModules()) {
                   it.second->destroy();
                 }
                 process->clear();

                 MPI_Finalize();
               }));

  m.def("clear", []() {
    auto process = RaNNCFactory::get();
    for (auto& it : process->getModules()) {
      it.second->destroy();
    }
    process->clear();
  });

  m.def("get_rank", []() { return mpi::getRank(); });

  m.def("get_world_size", []() { return mpi::getSize(); });

  m.def("barrier", []() { MPI_Barrier(MPI_COMM_WORLD); });

  m.def("delay_grad_allreduce", [](bool delay) {
    return RaNNCTensorBackward::setDelayGradAllreduce(delay);
  });

  m.def("sync_params_on_init", [](bool sync) {
    return ParamStorage::syncOnInit(sync);
  });

  m.def("recreate_all_communicators", []() {
    if (config::Config::get().getVal<bool>(config::RUN_WATCHDOG)) {
      SyncWatchDog& watch_dog = SyncWatchDog::get();
      watch_dog.start();
    }
    NCCLWrapper& nccl = NCCLWrapper::get();
    nccl.recreateAllCommunicators();
  });

  m.def("allreduce_tensor", [](py::handle py_tensor, bool sum) {
    auto iv = torch::jit::_toTypeInferredIValue(py_tensor);
    assert(iv.isTensor());

    NCCLWrapper& ar = NCCLWrapper::get();
    std::vector<at::Tensor> t = {iv.toTensor()};

    TagMap& tag_map = TagMap::get();
    int tag = tag_map.getRankSetTag(mpi::getAllRanks());

    ar.createCommunicator(tag, mpi::getAllRanks());
    if (sum) {
      ar.allreduce(tag, t);
    } else {
      ar.allreduceMin(tag, t);
    }
  });

  m.def("bcast_tensor", [](py::handle py_tensor, int root) {
    IRType ir_type;
    at::Tensor ten;
    if (mpi::getRank() == root) {
      auto iv = torch::jit::_toTypeInferredIValue(py_tensor);
      assert(iv.isTensor());
      ten = iv.toTensor().cuda();
      ir_type = toIRType(ten);
    }
    ObjectComm& ocomm = ObjectComm::get();
    ir_type = ocomm.bcast(ir_type, root);

    if (mpi::getRank() != root) {
      ten = createTensorFromIRType(ir_type, c10::Device(c10::DeviceType::CUDA));
    }

    NCCLWrapper& ar = NCCLWrapper::get();
    TagMap& tag_map = TagMap::get();
    int tag = tag_map.getRankSetTag(mpi::getAllRanks());

    ar.createCommunicator(tag, mpi::getAllRanks());
    ar.bcast(tag, {ten}, {root});
    return ten;
  });

  m.def("gather_tensor_zero", [](py::handle py_tensor, long param_id) {
    auto r = RaNNCFactory::get();
    auto param_storage = r->getParamStorage();
    auto iv = torch::jit::_toTypeInferredIValue(py_tensor);
    assert(iv.isTensor());
    const auto ten = iv.toTensor().cuda();
    return param_storage->gatherTensorZero(ten, param_id);
  });

  m.def("tensor_sliced", [](long param_id) {
    auto r = RaNNCFactory::get();
    auto param_storage = r->getParamStorage();
    return param_storage->sliced(param_id);
  });

  m.def("gather_tensor_sliced", [](py::handle py_tensor, long param_id) {
    auto r = RaNNCFactory::get();
    auto param_storage = r->getParamStorage();
    auto iv = torch::jit::_toTypeInferredIValue(py_tensor);
    assert(iv.isTensor());
    const auto ten = iv.toTensor().cuda();
    return param_storage->gatherTensorSliced(ten, param_id);
  });

  m.def(
      "keep_graph", [](bool keep) { return TorchDriver::setKeepGraph(keep); });

  m.def("dump_events", []() {
    EventRecorder& erec = EventRecorder::get();
    if (!erec.isEnabled()) {
      auto logger = getLogger("main");
      logger->warn("Event tracing has not been enabled. No event was output.");
      return;
    }
    erec.dump(
        config::Config::get().getVal<std::string>(config::EVENT_TRACE_FILE));
  });

  py::class_<RaNNCProcess, std::shared_ptr<RaNNCProcess>>(m, "RaNNCMaster")
      .def("start", [](RaNNCProcess& self) { self.start(); });

  m.def("get_rannc", []() { return RaNNCFactory::get(); });

  m.def("enter_rank", [](int rank) { enterRank(rank); });

  m.def("exit_rank", []() { exitRank(); });

  m.def("local_pid_to_global", [](long param_id) {
    auto r = RaNNCFactory::get();
    auto param_storage = r->getParamStorage();
    return param_storage->localToGlobal(param_id);
  });

  m.def(
      "register_amp_master_param", [](long model_param_id, py::object& param) {
        long master_param_id = getPythonObjId(param);
        const auto ten = py::cast<at::Tensor>(param);

        auto r = RaNNCFactory::get();
        auto param_storage = r->getParamStorage();
        param_storage->registerAmpMasterParam(
            model_param_id, master_param_id, ten);
      });

  m.def("store_dist_param", [](py::object& param) {
    long pid = getPythonObjId(param);
    const auto ten = py::cast<at::Tensor>(param);
    DistributedParamLocator& zpl = DistributedParamLocator::get();
    return zpl.store(pid, ten);
  });

  m.def("load_dist_param", [](long pid) {
    DistributedParamLocator& zpl = DistributedParamLocator::get();
    return zpl.load(pid);
  });

  m.def("set_dist_param", [](long pid, py::object& param) {
    DistributedParamLocator& zpl = DistributedParamLocator::get();
    const auto ten = py::cast<at::Tensor>(param);
    return zpl.set(pid, ten);
  });

  m.def("get_dist_param_segment", [](long pid) {
    DistributedParamLocator& zpl = DistributedParamLocator::get();
    return zpl.getSegment(pid);
  });

  m.def("set_dist_param_dtype", [](long pid, py::object& obj) {
    auto dtype = reinterpret_cast<THPDtype*>(obj.ptr());
    const auto stype = dtype->scalar_type;
    DistributedParamLocator& zpl = DistributedParamLocator::get();
    zpl.setScalarType(pid, stype);
  });

  m.def("get_dist_param_range", [](long pid) {
    DistributedParamLocator& zpl = DistributedParamLocator::get();
    return zpl.getSegmentRange(pid);
  });

  m.def("remove_dist_param", [](long pid) {
    DistributedParamLocator& zpl = DistributedParamLocator::get();
    return zpl.remove(pid);
  });

  m.def("dist_param_registered", [](long pid) {
    DistributedParamLocator& zpl = DistributedParamLocator::get();
    return zpl.registered(pid);
  });

  m.def("get_param_ranks", [](long pid) {
    auto r = RaNNCFactory::get();
    auto param_storage = r->getParamStorage();
    return param_storage->getRanks(pid);
  });

  m.def("set_tracing_state", [](bool enable) {
    static std::shared_ptr<torch::jit::tracer::TracingState> state;
    if (enable) {
      assert(!torch::jit::tracer::isTracing());
      if (state) {
        torch::jit::tracer::setTracingState(state);
        state.reset();
      } else {
        throw std::runtime_error("No valid tracing state is available.");
      }
    } else {
      assert(torch::jit::tracer::isTracing());
      assert(!state);
      state = torch::jit::tracer::getTracingState();
      torch::jit::tracer::setTracingState(nullptr);
    }
  });

  py::class_<RaNNCModule, std::shared_ptr<RaNNCModule>>(m, "RaNNCModule")
      .def(py::init<bool, bool, bool, bool, bool>())
      .def(
          "init",
          [](RaNNCModule& self, const py::function& fwdFunc,
             const std::vector<py::tuple>& params,
             const std::vector<py::tuple>& buffers,
             const py::function& var_lookup_fn, bool gather_inputs,
             const py::args& args) {
            try {
              return self.init(
                  fwdFunc, params, buffers, var_lookup_fn, args, gather_inputs);
            } catch (c10::Error& e) {
              std::cerr << "Torch exception caught: " << e.what() << std::endl;
              throw e;
            } catch (CommErrorException& e) {
              SyncWatchDog& watch_dog = SyncWatchDog::get();
              watch_dog.stop(true);
              throw e;
            } catch (std::runtime_error& e) {
              std::cerr << "Runtime error caught: " << e.what() << std::endl;
              throw e;
            } catch (std::invalid_argument& e) {
              std::cerr << "Invalid argument exception caught: " << e.what()
                        << std::endl;
              throw e;
            } catch (std::exception& e) {
              std::cerr << "Unknown exception caught: " << e.what()
                        << std::endl;
              throw e;
            }
            std::cerr << "Failed to init RaNNC." << std::endl;
            throw std::runtime_error("Failed to init RaNNC.");
          })
      .def(
          "__call__",
          [](RaNNCModule& self, py::args args, py::kwargs kwargs) {
            try {
              return self(args, kwargs);
            } catch (c10::Error& e) {
              std::cerr << "Torch exception caught: " << e.what() << std::endl;
              throw e;
            } catch (CommErrorException& e) {
              SyncWatchDog& watch_dog = SyncWatchDog::get();
              watch_dog.stop(true);
              throw e;
            } catch (std::runtime_error& e) {
              std::cerr << "Runtime error caught: " << e.what() << std::endl;
              throw e;
            } catch (std::invalid_argument& e) {
              std::cerr << "Invalid argument exception caught: " << e.what()
                        << std::endl;
              throw e;
            } catch (std::exception& e) {
              std::cerr << "Unknown exception caught: " << e.what()
                        << std::endl;
              throw e;
            }
            std::cerr << "Failed to compute forward." << std::endl;
            throw std::runtime_error("Failed to compute forward.");
          })
      .def(
          "allreduce_grads",
          [](RaNNCModule& self) { self.allReduceParamGrads(); })
      .def(
          "allreduce_grads_zero",
          [](RaNNCModule& self, double loss_scale) {
            self.allReduceParamGradsZero(loss_scale);
          })
      .def(
          "clip_grad_norm",
          [](RaNNCModule& self, float max_grad_norm) {
            self.clipGrad(max_grad_norm);
          })
      .def(
          "calc_grad_norm",
          [](RaNNCModule& self) { return self.calcGradL2Norm(); })
      .def(
          "enable_dropout",
          [](RaNNCModule& self, bool enable) { self.enableDropout(enable); })
      .def(
          "is_checkpointing_enabled",
          [](RaNNCModule& self) { return self.isCheckpointingEnabled(); })
      .def("zero_grad", [](RaNNCModule& self) { self.clearParamGrads(); })
      .def(
          "use_amp_master_params",
          [](RaNNCModule& self) { return self.useAmpMasterParams(); })
      .def("undeploy", [](RaNNCModule& self) { self.destroy(); })
      .def(
          "sync_param",
          [](RaNNCModule& self, long param_id) {
            return self.syncParam(param_id);
          })
      .def(
          "sync_param_grad",
          [](RaNNCModule& self, long param_id) {
            return self.syncParamGrad(param_id);
          })
      .def(
          "sync_param_zero",
          [](RaNNCModule& self, bool grad) { return self.syncParamZero(grad); })
      .def(
          "get_local_param_range",
          [](RaNNCModule& self, long param_id) {
            return self.getLocalParamRange(param_id);
          })
      .def(
          "get_local_param_segment",
          [](RaNNCModule& self, long param_id) {
            return self.getLocalParamSegment(param_id);
          })
      .def(
          "get_param",
          [](RaNNCModule& self, long param_id, long amp_master_param) {
            return self.getParam(param_id, amp_master_param);
          })
      .def(
          "get_param_grad",
          [](RaNNCModule& self, long param_id, long amp_master_param) {
            return self.getParamGrad(param_id, amp_master_param);
          })
      .def(
          "load_deployment",
          [](RaNNCModule& self, const std::string& file) {
            self.setLoadDeployment(true);
            self.setDeploymentFile(file);
          })
      .def(
          "save_deployment",
          [](RaNNCModule& self, const std::string& file) {
            self.saveDeployment(file);
          })
      .def("__del__", [](RaNNCModule& self) { self.destroy(); });

  m.def("bcast_bytes", [](const py::bytes& data, int root) {
    std::string str_data = static_cast<std::string>(data);

    ObjectComm& ocomm = ObjectComm::get();
    const std::string recv_data = ocomm.bcast(str_data, root, MPI_COMM_WORLD);
    return py::bytes(recv_data);
  });

  m.def("run_dp_dry", [](const std::string& path) {
    DPStagingCache cache = loadFromFile<DPStagingCache>(path);
    DPDryStaging dp(cache);
    const auto deployment = dp.partition();

    const auto deployment_file =
        config::Config::get().getVal<std::string>(config::DEPLOYMENT_FILE);
    spdlog::info("Saving deployment state to {}", deployment_file);
    save(deployment_file, deployment, cache.conf.dev_num, cache.conf.dev_mem);
  });

  m.def("show_deployment", [](const std::string& path, int64_t batch_size) {
    spdlog::info("Loading deployment state from {}", path);
    const DeploymentState state = loadDeploymentState(path);
    const Deployment& deployment = state.deployment;
    std::stringstream ss;
    ss << deployment;

    std::unordered_set<int> all_ranks;
    for (const auto& it : deployment.allocation) {
      for (int r : it.second) {
        all_ranks.insert(r);
      }
    }

    BatchSizeCalculator bs_calc(deployment.pipeline_num, batch_size);

    for (int i = 0; i < deployment.pipeline_num; i++) {
      int64_t split_bs = bs_calc.getGlobalSplitBatchSize(i);
      ss << "split" << i << ": bs=" << split_bs;

      std::vector<std::string> local_splits_str;
      for (int r = 0; r < all_ranks.size(); r++) {
        std::stringstream split_ss;
        split_ss << r << "=" << bs_calc.getLocalSplitBatchSize(all_ranks, r, i);
        local_splits_str.push_back(split_ss.str());
      }
      ss << " local_splits=" << join_as_str(local_splits_str) << std::endl;

      for (const auto& sg_name : deployment.fwd_graph_order) {
        const auto& g = deployment.subgraphs.at(sg_name);
        auto alloc = setToVector(deployment.allocation.at(sg_name));
        std::sort(alloc.begin(), alloc.end());
        ss << " " << sg_name << " repl_num=" << alloc.size()
           << " alloc=" << join_as_str(alloc);

        std::vector<std::string> sg_splits_str;
        for (int r : alloc) {
          std::stringstream split_ss;
          split_ss << r << "="
                   << bs_calc.getLocalSplitBatchSize(vectorToSet(alloc), r, i);
          sg_splits_str.push_back(split_ss.str());
        }
        ss << " bs=" << join_as_str(sg_splits_str) << std::endl;
      }
    }

    spdlog::info(ss.str());
  });

  m.def(
      "matmul_dist",
      [](py::handle py_tensor1, py::handle py_tensor2,
         const std::string& type) {
        auto iv1 = torch::jit::_toTypeInferredIValue(py_tensor1);
        assert(iv1.isTensor());
        at::Tensor ten1 = iv1.toTensor().cuda();

        auto iv2 = torch::jit::_toTypeInferredIValue(py_tensor2);
        assert(iv2.isTensor());
        at::Tensor ten2 = iv2.toTensor().cuda();

        DistMatmul dist_mm;
        if (type == "RRR") {
          return dist_mm.runRRR_AG(ten1, ten2, mpi::getAllRanks());
        } else if (type == "RCR") {
          return dist_mm.runRCR_AG(ten1, ten2, mpi::getAllRanks());
        } else if (type == "CRC") {
          return dist_mm.runCRC(ten1, ten2, mpi::getAllRanks());
        }
        spdlog::info("No match: {}", type);
        return at::Tensor();
      });

  m.def("test_gather", [](py::handle py_tensor, int64_t dim) {
    auto iv = torch::jit::_toTypeInferredIValue(py_tensor);
    assert(iv.isTensor());
    at::Tensor ten = iv.toTensor().cuda();

    std::vector<int64_t> ranks;
    for (int r : mpi::getAllRanks()) {
      ranks.push_back(r);
    }
    std::sort(ranks.begin(), ranks.end());

    return GatherFunction::apply(ten, dim, ranks);
  });

  m.def("abort_all_processes", []() {
    NCCLWrapper& nccl = NCCLWrapper::get();
    nccl.abortAllCommunicators();
    nccl.destroyAllCommunicators();
    MPI_Abort(MPI_COMM_WORLD, -1);
  });

#ifdef VERSION_INFO
  m.attr("__version__") = VERSION_INFO;
#else
  m.attr("__version__") = "dev";
#endif
}
