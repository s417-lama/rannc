import copy
import random
import sys

import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
import torch.distributed as dist

from . import common
from apex import amp

import pyrannc
from pyrannc.amp import allreduce_grads, allreduce_grads_rannc
from pyrannc.opt.util import gather_optimizer_state_dict

ASSERT_DECIMAL = 3
seed = 0
RELATIVE_TOLERANCE = common.RELATIVE_TOLERANCE
ABSOLUTE_TOLERANCE = common.ABSOLUTE_TOLERANCE
LOSS_SCALE = 2**4


if not torch.cuda.is_available():
    print("This test is valid only on a cuda environment.")
    sys.exit(0)


class Net(nn.Module):

    INPUT_DIM = (3,)
    OUTPUT_DIM = (3,)

    def __init__(self):
        super(Net, self).__init__()
        self.fc1 = nn.Linear(3, 2, bias=False)
        w1 = torch.tensor([[0.1, 0.2, 0.3], [0.4, 0.5, 0.6]], requires_grad=True)
        self.fc1.weight = torch.nn.Parameter(w1)
        self.fc2 = nn.Linear(2, 3, bias=False)
        w2 = torch.tensor([[0.7, 0.8], [0.9, 1.0], [1.1, 1.2]], requires_grad=True)
        self.fc2.weight = torch.nn.Parameter(w2)
        self.criterion = nn.MSELoss()

    def forward(self, x, tgt):
        x = self.fc1(x)
        x = self.fc2(x)
        loss = self.criterion(x, tgt)
        return loss


def do_run(model_base, batch_size_per_proc, input_dim, output_dim, num_iter,
           fp16, rtol, atol, get_dataset,
           **kwargs):

    device = torch.device("cuda")
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    torch.cuda.manual_seed(seed)

    data_loader = common.get_loader(
        batch_size_per_proc, input_dim, output_dim, num_iter, get_dataset)

    lr = 0.01

    #model_base = Net().to(device)
    model_base = model_base.to(device)
    rmodel_base =copy.deepcopy(model_base)

    opt_base = optim.Adam(model_base.parameters(), lr=lr)
    model, opt = amp.initialize(model_base, opt_base, opt_level="O2",
                                loss_scale=LOSS_SCALE, master_weights=True)
    model = torch.nn.parallel.DistributedDataParallel(model, device_ids=[pyrannc.get_rank()],
                                                      output_device=pyrannc.get_rank())

    ropt_base = optim.Adam(rmodel_base.parameters(), lr=lr)
    rmodel_base, ropt = amp.initialize(rmodel_base, ropt_base, opt_level="O2",
                                       loss_scale=LOSS_SCALE, master_weights=True)
    rmodel = pyrannc.RaNNCModule(rmodel_base, ropt, use_amp_master_params=True)

    # we manually run allreduce
    pyrannc.delay_grad_allreduce(True)

    for x, tgt in data_loader:
        # Create test input
        x = x.to(device)
        tgt = tgt.to(device)

        p_loss = model(x, tgt)
        tmp_loss = p_loss.clone()
        torch.distributed.all_reduce(tmp_loss)
        tmp_loss /= pyrannc.get_world_size()

        r_loss = rmodel(x, tgt)

        # Verify the equality of outputs
        np.testing.assert_equal(tmp_loss.size(), r_loss.size())
        np.testing.assert_almost_equal(tmp_loss.tolist(), r_loss.tolist(), decimal=ASSERT_DECIMAL)

        with amp.scale_loss(p_loss, opt, delay_overflow_check=False, delay_unscale=False) as scaled_loss:
            scaled_loss.backward()
        torch.nn.utils.clip_grad_norm_(amp.master_params(opt), 1.0)

        with amp.scale_loss(r_loss, ropt, delay_overflow_check=False, delay_unscale=False) as scaled_loss:
            scaled_loss.backward()
        allreduce_grads_rannc(rmodel, ropt)
        rmodel.clip_grad_norm(1.0)

        expected_master_params = {n: p for n, p in zip([n for n, p in model.module.named_parameters()], amp.master_params(opt))}
        actual_master_params = {n: p for n, p in zip([n for n, p in rmodel.named_parameters()], amp.master_params(ropt))}
        for n, rp in actual_master_params.items():
            p = expected_master_params[n]
            np.testing.assert_equal(rp.grad.size(), p.grad.size())
            np.testing.assert_almost_equal(rp.grad.tolist(), p.grad.tolist(), decimal=ASSERT_DECIMAL)

        opt.step()
        ropt.step()

        for n, rp in actual_master_params.items():
            p = expected_master_params[n]
            np.testing.assert_equal(rp.size(), p.size())
            np.testing.assert_almost_equal(rp.tolist(), p.tolist(), decimal=ASSERT_DECIMAL)

        opt.zero_grad()
        ropt.zero_grad()

    pyrannc.clear()

    # Save model & opt
    # state_dict should run on all ranks
    model_state_dict = rmodel.state_dict()
    global_opt_state_dict = ropt.state_dict(from_global=True)
    # global_state, param_ranks = gather_optimizer_state_dict(ropt, use_amp_master_param=True)

    if pyrannc.get_rank() == 0:
        torch.save(model_state_dict, 'model.pt')
        torch.save(global_opt_state_dict, 'opt_state.pt')

    pyrannc.barrier()

    ld_model = Net().to(device)
    ld_opt = optim.Adam(ld_model.parameters(), lr=lr)
    ld_model, ld_opt = amp.initialize(ld_model, ld_opt, opt_level="O2",
                                      loss_scale="dynamic", master_weights=True)
    ld_model.load_state_dict(torch.load('model.pt'))
    ld_model = pyrannc.RaNNCModule(ld_model, ld_opt, use_amp_master_params=True)

    global_opt_state_dict = torch.load('opt_state.pt')

    # Verify parameters
    for p1, p2 in zip(rmodel.parameters(), ld_model.parameters()):
        np.testing.assert_equal(p1.size(), p2.size())
        np.testing.assert_almost_equal(p1.tolist(), p2.tolist(), decimal=ASSERT_DECIMAL)

    for x, tgt in data_loader:
        x = x.to(device)
        tgt = tgt.to(device)
        p_out = ld_model(x, tgt)

    # Note that restoring optimizer's state follows the decomposition
    ld_opt.load_state_dict(global_opt_state_dict, from_global=True)

    ropt_state_dict = ropt.state_dict()
    ld_opt_state_dict = ld_opt.state_dict()

    for ld_grp, r_grp in zip(ld_opt_state_dict['param_groups'], ropt_state_dict['param_groups']):
        np.testing.assert_(ld_grp.keys(), r_grp.keys())
        for k in r_grp.keys():
            if k == 'params':
                np.testing.assert_equal(len(ld_grp['params']), len(r_grp['params']))
            else:
                np.testing.assert_(ld_grp[k] == r_grp[k])

        for ld_pid, r_pid in zip(ld_grp['params'], r_grp['params']):
            ld_param_state = ld_opt_state_dict['state'][ld_pid]
            r_param_state = ropt_state_dict['state'][r_pid]
            np.testing.assert_(ld_param_state.keys(), r_param_state.keys())
            for k in r_param_state.keys():
                ldv = ld_param_state[k]
                rv = r_param_state[k]
                if isinstance(ldv, torch.Tensor):
                    np.testing.assert_equal(ldv.size(), rv.size())
                    np.testing.assert_almost_equal(ldv.tolist(), rv.tolist(), decimal=ASSERT_DECIMAL)
                else:
                    np.testing.assert_(ldv == rv)

    print("Done")

def run(model_base, batch_size_per_proc, num_iter,
        fp16=False,
        rtol=common.RELATIVE_TOLERANCE,
        atol=common.ABSOLUTE_TOLERANCE,
        get_dataset=None,
        **kwargs):
    do_run(model_base, batch_size_per_proc,
           model_base.INPUT_DIM, model_base.OUTPUT_DIM, num_iter,
           #lambda model, x, tgt: torch.jit.trace(model, (x,)),
           #lambda model, x, tgt: model(x),
           #lambda out: out,
           #bwd_with_criterion,
           fp16, rtol, atol, get_dataset,
           **kwargs)

def test_half_loss_amp(init_dist, batch_size, iteration):
    print("test_half_loss_amp_save")
    run(Net(), batch_size, iteration)
