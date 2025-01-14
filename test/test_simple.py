import pytest

from . import common, models

# from . import native_models

default_vals = {
    "rtol": {
        "fp16": 1e-1,
        "fp32": 1e-1
    },
    "atol": {
        "fp16": 1e-2,
        "fp32": 0.
    },
    "get_dataset": None,
    "loss_out": False,
    "preprocess": None
}

test_models = [
    {"model": models.SmallParamModel},
    {"model": models.SharedInputModel},
    {"model": models.ForkJoinModel},
    {"model": models.SharedParamModel},
    {"model": models.OneOpModel},
    {"model": models.TensorMulModel},
    {"model": models.EmbeddingModel, "get_dataset": models.EmbeddingModel.get_dataset},
    {"model": models.FunctionModel, "get_dataset": models.FunctionModel.get_dataset},
    {"model": models.LossOutModel, "loss_out": True},
    {"model": models.BasicModel, "atol": {"fp16": 5e-2, "fp32": 0.}, },
    # {"model": native_models.NativeCallModel01}, # compiles module
    # {"model": models.LayerNormModel, "preprocess": models.norm_to_float} # DP only
]


@pytest.mark.parametrize("test_model", test_models)
@pytest.mark.parametrize("gradient_accumulation_steps", [1, 2, 4])
@pytest.mark.parametrize("use_amp", [False, True])
@pytest.mark.parametrize("allreduce_amp_master_params", [False, True])
@pytest.mark.parametrize("enable_zero", [False, True])
@pytest.mark.parametrize("dist_params", [False, True])
@pytest.mark.parametrize("offload_params", [False])
def test_match(init_dist, init_seed, batch_size, iteration, test_model, gradient_accumulation_steps,
               use_amp, allreduce_amp_master_params,
               enable_zero, dist_params, offload_params):
    if allreduce_amp_master_params and (not use_amp):
        print("use_amp must be True if allreduce_amp_master_params == True")
        return

    if enable_zero and (not allreduce_amp_master_params):
        print("allreduce_amp_master_params must be True if enable_zero == True")
        return

    print("model={} use_amp={} allreduce_amp_master_params={} enable_zero={} dist_params={}"
          " gradient_accumulation_steps={} offload_params={}".format(
        test_model["model"].__name__,
        use_amp, allreduce_amp_master_params, enable_zero, dist_params, gradient_accumulation_steps, offload_params))

    for k, v in default_vals.items():
        if k not in test_model:
            test_model[k] = v

    common.run(test_model["model"], batch_size, iteration,
               loss_out=test_model["loss_out"],
               preprocess=test_model["preprocess"],
               gradient_accumulation_steps=gradient_accumulation_steps,
               use_amp=use_amp,
               allreduce_amp_master_params=allreduce_amp_master_params,
               enable_zero=enable_zero,
               dist_params=dist_params,
               offload_params=offload_params,
               rtol=test_model["rtol"],
               atol=test_model["atol"],
               get_dataset=test_model["get_dataset"]
               )
