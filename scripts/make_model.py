#!/usr/bin/env python3
"""make_model.py — build the tiny CNN "policy" as an ONNX graph, no PyTorch needed.

Architecture (input = one 160x120 RGB image, output = a 7-D action, like a 7-DoF arm):

    image [1,3,120,160]
      -> Conv 3->16, 3x3, stride 2 -> ReLU      [1,16,60,80]
      -> Conv 16->32, 3x3, stride 2 -> ReLU     [1,32,30,40]
      -> Conv 32->64, 3x3, stride 2 -> ReLU     [1,64,15,20]
      -> GlobalAveragePool -> Flatten           [1,64]
      -> Gemm 64->7                             [1,7]   "action"

Weights are random (He init, fixed seed): we measure *latency*, not task accuracy,
and the latency of a conv net does not depend on what its weights are (with denormals
flushed — see policy_model.hpp). The model is deliberately small so the experiment
measures the pipeline, not a CPU-bound giant network. Swap in MobileNetV2 with
`fp_consumer --model path/to/mobilenetv2.onnx` to see a heavier load.
"""
import argparse
from pathlib import Path

import numpy as np
import onnx
from onnx import TensorProto, helper, numpy_helper


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="models/tiny_policy.onnx")
    ap.add_argument("--height", type=int, default=120)
    ap.add_argument("--width", type=int, default=160)
    args = ap.parse_args()

    rng = np.random.default_rng(0)
    inits, nodes = [], []

    def conv(name, cin, cout, inp, out):
        w = (rng.standard_normal((cout, cin, 3, 3)) * np.sqrt(2.0 / (cin * 9))).astype(np.float32)
        b = np.zeros(cout, dtype=np.float32)
        inits.extend([numpy_helper.from_array(w, f"{name}_w"), numpy_helper.from_array(b, f"{name}_b")])
        nodes.append(helper.make_node("Conv", [inp, f"{name}_w", f"{name}_b"], [f"{name}_c"],
                                      kernel_shape=[3, 3], strides=[2, 2], pads=[1, 1, 1, 1]))
        nodes.append(helper.make_node("Relu", [f"{name}_c"], [out]))

    conv("c1", 3, 16, "image", "r1")
    conv("c2", 16, 32, "r1", "r2")
    conv("c3", 32, 64, "r2", "r3")
    nodes.append(helper.make_node("GlobalAveragePool", ["r3"], ["gap"]))
    nodes.append(helper.make_node("Flatten", ["gap"], ["feat"], axis=1))
    wf = (rng.standard_normal((7, 64)) * np.sqrt(1.0 / 64)).astype(np.float32)
    inits.extend([numpy_helper.from_array(wf, "fc_w"), numpy_helper.from_array(np.zeros(7, np.float32), "fc_b")])
    nodes.append(helper.make_node("Gemm", ["feat", "fc_w", "fc_b"], ["action"], transB=1))

    graph = helper.make_graph(
        nodes, "tiny_policy",
        [helper.make_tensor_value_info("image", TensorProto.FLOAT, [1, 3, args.height, args.width])],
        [helper.make_tensor_value_info("action", TensorProto.FLOAT, [1, 7])],
        inits,
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], producer_name="framepipe")
    model.ir_version = 8  # readable by ONNX Runtime >= 1.10
    onnx.checker.check_model(model)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(model, str(out))
    n_params = sum(int(np.prod(t.dims)) for t in inits)
    print(f"wrote {out}  ({n_params:,} parameters, input [1,3,{args.height},{args.width}], output [1,7])")


if __name__ == "__main__":
    main()
