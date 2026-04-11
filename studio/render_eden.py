#!/usr/bin/env python3
"""
SwarmRT Video Studio — Eden Garden render
HunyuanVideo-1.5 (8.3B) — fast + high quality
"""

import os
import time
import gc
import torch

PROMPT = (
    "Cinematic wide establishing shot of a mythical paradise garden at golden hour. "
    "Lush crimson and deep red grass covers rolling hills beneath ancient cherry blossom trees "
    "with pink and white petals drifting through warm golden sunlight. "
    "Crystal clear streams wind through the garden with smooth mossy stones. "
    "Three beautiful young East Asian women in flowing silk kimono robes with elegant floral patterns, "
    "sheer fabric catching the light, run playfully barefoot through the red grass, "
    "laughing with long black hair flowing behind them. "
    "Dramatic volumetric god rays filter through the canopy. "
    "Ethereal mist rises from the streams. Fireflies and golden particles float in the air. "
    "Shot on ARRI Alexa with anamorphic lens, shallow depth of field, "
    "warm amber and magenta color grading. Dreamlike and cinematic."
)


def main():
    from diffusers import HunyuanVideo15Pipeline
    from diffusers.utils import export_to_video

    output_dir = "studio/output/video_1/renders"
    os.makedirs(output_dir, exist_ok=True)
    out_path = os.path.join(output_dir, "eden_garden.mp4")

    print("=" * 60)
    print("  Eden Garden — HunyuanVideo-1.5 (8.3B)")
    print("=" * 60)
    for i in range(torch.cuda.device_count()):
        p = torch.cuda.get_device_properties(i)
        print(f"  GPU {i}: {p.name} ({p.total_memory/1e9:.0f} GB)")
    print()

    print("[1/2] Loading HunyuanVideo-1.5 pipeline...")
    t0 = time.time()
    pipe = HunyuanVideo15Pipeline.from_pretrained(
        "hunyuanvideo-community/HunyuanVideo-1.5-Diffusers-480p_t2v",
        torch_dtype=torch.bfloat16,
    )
    pipe.vae.enable_tiling()
    pipe.enable_model_cpu_offload()
    print(f"  Pipeline loaded in {time.time()-t0:.1f}s")

    print()
    print("[2/2] Rendering eden garden...")
    print(f"  Prompt: {PROMPT[:80]}...")

    t0 = time.time()
    generator = torch.Generator(device="cpu").manual_seed(77)

    result = pipe(
        prompt=PROMPT,
        height=544,
        width=960,
        num_frames=97,
        num_inference_steps=30,
        generator=generator,
    )

    video = result.frames[0]
    export_to_video(video, out_path, fps=24)

    elapsed = time.time() - t0
    size_mb = os.path.getsize(out_path) / 1024 / 1024
    print(f"  Done in {elapsed:.1f}s -> {out_path} ({size_mb:.1f} MB)")

    del result, pipe
    gc.collect()
    torch.cuda.empty_cache()

    print()
    print("=" * 60)
    print(f"  RENDER COMPLETE: {out_path}")
    print("=" * 60)


if __name__ == "__main__":
    main()
