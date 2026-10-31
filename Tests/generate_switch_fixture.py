"""Export models and reference audio for every combination of three switches."""

import argparse
import itertools
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
    from nam.capture.params import KnobSpec, SwitchSpec
    from nam.models.parametric import ConcatWaveNet, HyperA2, export_parametric

    torch.set_num_threads(1)
    torch.manual_seed(3061)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    controls = [
        SwitchSpec("Mode", ("Clean", "Crunch", "Lead"), default=1),
        KnobSpec("Gain", 0, 10, default=2),
        SwitchSpec("Bright", ("Off", "On"), default=1),
        KnobSpec("Tone", 0, 10, default=5),
        SwitchSpec("Voice", ("Vintage", "Classic", "Modern", "Deep", "Flat"), default=3),
    ]
    specs = [control.to_param_spec().to_dict() for control in controls]
    settings = [
        [float(mode), 2.0 + 6.0 * (i % 2), float(bright), 2.5 * (i % 5), float(voice)]
        for i, (mode, bright, voice) in enumerate(itertools.product(range(3), range(2), range(5)))
    ]
    signal = 0.1 * torch.sin(torch.arange(1024) * 0.028) + 0.03 * torch.cos(torch.arange(1024) * 0.13)
    a2 = HyperA2.init_from_config({
        "params": specs, "sample_rate": 48000, "head_scale": 0.2,
        "hypernet": {"mode": "low_rank", "rank": 2, "hidden_sizes": [8],
                     "selector": {"exclude_suffixes": ["_conv.weight"]}},
    }).eval()
    with torch.no_grad():
        for size in a2.sizes:
            a2.branch(size)._hypernet._final.weight.normal_(0, 0.05)
    concat = ConcatWaveNet.init_from_config({
        "params": specs, "sample_rate": 48000,
        "layers": [{"channels": 4, "kernel_size": 3, "dilations": [1, 2],
                    "activation": "LeakyReLU",
                    "head": {"out_channels": 1, "kernel_size": 1, "bias": True}}],
        "head_scale": 0.2,
    }).eval()
    cases = []
    for name, model in (("switch_a2", a2), ("switch_hyper", a2.branch("full")),
                        ("switch_concat", concat)):
        expected = {}
        for size in (a2.sizes if model is a2 else ("full",)):
            if model is a2:
                model.set_size(size)
            with torch.no_grad():
                audio = torch.stack([model(signal, torch.tensor(p)) for p in settings])
                # A fixture that sounds identical for two settings could hide a routing bug.
                separation = min((audio[i] - audio[j]).abs().max().item()
                                 for i in range(len(settings)) for j in range(i))
                if separation <= 4e-5:
                    raise RuntimeError(f"{name}/{size}: ambiguous reference outputs ({separation})")
                expected[size] = audio.tolist()
        export_parametric(model, out, basename=name)
        cases.append({"model": name + ".nam", "dual_size": model is a2, "expected": expected})
    (out / "switch_combinations.json").write_text(json.dumps({
        "input": signal.tolist(), "params": specs, "settings": settings, "cases": cases,
    }), encoding="utf-8")
    print(f"Exported {len(settings)} joint switch settings for HyperA2 Full/Lite, HyperWaveNet and ConcatWaveNet")


if __name__ == "__main__":
    main()
