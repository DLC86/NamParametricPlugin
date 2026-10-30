"""Generate actual trainer exports and reference audio for C++ integration tests."""

import argparse
import json
from pathlib import Path
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trainer-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sys.path.insert(0, str(args.trainer_dir.resolve()))

    import torch
    from nam.models.parametric import ConcatWaveNet, HyperA2, export_parametric

    torch.set_num_threads(1)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    params = [
        {"name": "Gain", "type": "continuous", "min": 0, "max": 10, "default": 2},
        {"name": "Boost", "type": "switch", "min": 0, "max": 1, "default": 0,
         "enum_names": ["Off", "On"]},
    ]
    signal = 0.1 * torch.sin(torch.arange(4096) * 0.028) + 0.03 * torch.cos(torch.arange(4096) * 0.13)
    settings = [[2.0, 0.0], [8.0, 1.0]]
    cases = []
    for index, mode in enumerate(("low_rank", "full")):
        torch.manual_seed(814 + index)
        model = HyperA2.init_from_config({
            "params": params, "sample_rate": 48000,
            "hypernet": {"mode": mode, "rank": 2, "hidden_sizes": [4],
                         "selector": {"exclude_suffixes": ["_conv.weight"]}},
        }).eval()
        with torch.no_grad():
            for size in model.sizes:
                model.branch(size)._hypernet._final.weight.normal_(0, 0.01)
        expected = {}
        calibrated_expected = {}
        for size in model.sizes:
            model.set_size(size)
            with torch.no_grad():
                expected[size] = [model(signal, torch.tensor(p)).tolist() for p in settings]
                calibrated_expected[size] = (
                    model(signal * (10.0 ** (-6.0 / 20)), torch.tensor(settings[0]))
                    * (10.0 ** (-6.0 / 20))
                ).tolist()
        # Deliberately export Lite as the trainer preference. The plugin must still
        # default to Full unless the user explicitly selects/restores Lite.
        model.set_size("lite")
        name = f"hyper_a2_{mode}"
        export_parametric(model, out, basename=name)
        cases.append({"model": name + ".nam", "expected": expected,
                      "calibrated_expected": calibrated_expected})
        if index == 0:
            export_parametric(model.branch("full"), out, basename="hyper_reference")

    torch.manual_seed(125)
    concat = ConcatWaveNet.init_from_config({
        "params": [{"name": f"Control{i}", "type": "continuous", "min": 0,
                    "max": 10, "default": 5} for i in range(5)],
        "sample_rate": 48000,
        "layers": [{"channels": 4, "kernel_size": 3, "dilations": [1, 2],
                    "activation": "LeakyReLU",
                    "head": {"out_channels": 1, "kernel_size": 1, "bias": True}}],
        "head_scale": 0.01,
    }).eval()
    export_parametric(concat, out, basename="concat_reference")
    (out / "manifest.json").write_text(json.dumps({
        "input": signal.tolist(), "params": settings, "cases": cases,
    }), encoding="utf-8")
    print(f"Wrote trainer exports and reference audio to {out}")


if __name__ == "__main__":
    main()
