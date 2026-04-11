#!/usr/bin/env python3
"""
Eden Garden — Wan 2.2 14B (MoE, only 14B active per step)
Uses diffusers WanPipeline — PyTorch SDPA, no flash-attn needed.
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

NEGATIVE = (
    "Bright tones, overexposed, static, blurred details, subtitles, watermark, "
    "low quality, distorted, cartoon, anime, ugly, deformed"
)


def main():
    from diffusers import AutoencoderKLWan, WanPipeline
    from diffusers.utils import export_to_video

    output_dir = "studio/output/video_1/renders"
    os.makedirs(output_dir, exist_ok=True)
    out_path = os.path.join(output_dir, "eden_wan22_v2.mp4")

    print("=" * 60)
    print("  Eden Garden — Wan 2.2 14B MoE")
    print("=" * 60)
    for i in range(torch.cuda.device_count()):
        p = torch.cuda.get_device_properties(i)
        print(f"  GPU {i}: {p.name} ({p.total_memory/1e9:.0f} GB)")
    print()

    model_id = "Wan-AI/Wan2.2-T2V-A14B-Diffusers"

    print("[1/3] Loading VAE...")
    t0 = time.time()
    vae = AutoencoderKLWan.from_pretrained(
        model_id, subfolder="vae", torch_dtype=torch.float32
    )
    print(f"  VAE loaded in {time.time()-t0:.1f}s")

    print("[2/3] Loading Wan 2.2 14B pipeline...")
    t0 = time.time()
    pipe = WanPipeline.from_pretrained(
        model_id, vae=vae, torch_dtype=torch.bfloat16
    )
    pipe.vae.enable_tiling()
    pipe.enable_model_cpu_offload()
    print(f"  Pipeline loaded in {time.time()-t0:.1f}s")

    print("[3/3] Rendering...")
    print(f"  Prompt: {PROMPT[:80]}...")
    t0 = time.time()
    generator = torch.Generator(device="cpu").manual_seed(77)

    result = pipe(
        prompt=PROMPT,
        negative_prompt=NEGATIVE,
        height=720,
        width=1280,
        num_frames=81,
        num_inference_steps=50,
        guidance_scale=7.5,
        generator=generator,
    )

    video = result.frames[0]
    export_to_video(video, out_path, fps=16)

    elapsed = time.time() - t0
    size_mb = os.path.getsize(out_path) / 1024 / 1024
    print(f"  Done in {elapsed:.1f}s -> {out_path} ({size_mb:.1f} MB)")

    del result, pipe
    gc.collect()
    torch.cuda.empty_cache()

    print(f"\n  RENDER COMPLETE: {out_path}")


if __name__ == "__main__":
    main()
