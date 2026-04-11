#!/usr/bin/env python3
"""
SwarmRT Video Studio — HunyuanVideo Renderer
Uses HunyuanVideo 13B via diffusers for high-quality cinematic video generation.
Designed for 2x RTX PRO 6000 Blackwell (96GB each).
"""

import argparse
import os
import sys
import time
import gc
import torch

SCENES = [
    {
        "name": "scene_1_chip",
        "prompt": (
            "Cinematic macro shot of a glowing silicon microchip on a dark reflective surface. "
            "Thousands of tiny golden traces and circuits pulse with warm amber light. "
            "The camera slowly dollies forward into the surface of the chip, revealing "
            "intricate layers of copper pathways and transistor grids. Shallow depth of field, "
            "dark moody lighting with orange and teal rim lights. Shot on ARRI Alexa, "
            "anamorphic lens flare. 4K cinematic film grain."
        ),
        "num_frames": 129,
        "guidance_scale": 6.0,
    },
    {
        "name": "scene_2_datacenter",
        "prompt": (
            "Steadicam tracking shot moving through a dark futuristic server room. "
            "Rows of tall black server racks stretch into the distance, each blinking "
            "with hundreds of small blue and white LED indicator lights. Cool blue fog "
            "drifts low across the floor. Overhead cable trays with fiber optic cables "
            "glow faintly purple. Volumetric light beams cut through the haze. "
            "Cinematic sci-fi atmosphere, clean and minimal. 4K, shallow depth of field."
        ),
        "num_frames": 129,
        "guidance_scale": 6.0,
    },
    {
        "name": "scene_3_sparks",
        "prompt": (
            "Extreme close-up of a CNC machine precisely milling a piece of brushed aluminum. "
            "Bright orange sparks fly in slow motion from the cutting tool. "
            "The metal surface reflects cool blue workshop lighting. Shallow depth of field, "
            "the background is dark and out of focus. Tiny curls of metal shavings peel away. "
            "Industrial craftsmanship, precise and powerful. Shot at 120fps slow motion, "
            "cinematic color grading with orange and teal contrast."
        ),
        "num_frames": 97,
        "guidance_scale": 6.0,
    },
    {
        "name": "scene_4_earth",
        "prompt": (
            "Aerial view of Earth from low orbit at night. City lights form glowing golden "
            "networks across continents, connected by thin bright lines representing data "
            "flowing between cities. The atmosphere glows with a thin blue haze at the horizon. "
            "Stars visible in the black sky above. The camera slowly rotates, revealing more "
            "of the illuminated planet. Photorealistic, NASA-style footage, cinematic and awe-inspiring. "
            "Deep blacks, warm city glow, cool blue atmosphere."
        ),
        "num_frames": 129,
        "guidance_scale": 6.0,
    },
]


def load_pipeline():
    """Load HunyuanVideo pipeline from HuggingFace."""
    from diffusers import HunyuanVideoPipeline, HunyuanVideoTransformer3DModel

    model_id = "hunyuanvideo-community/HunyuanVideo"

    print("[1/3] Loading HunyuanVideo transformer (13B)...")
    print("  This will download ~25GB on first run...")
    t0 = time.time()
    transformer = HunyuanVideoTransformer3DModel.from_pretrained(
        model_id,
        subfolder="transformer",
        torch_dtype=torch.bfloat16,
    )
    print(f"  Transformer loaded in {time.time()-t0:.1f}s")

    print("[2/3] Loading full pipeline (text encoder + VAE + scheduler)...")
    t0 = time.time()
    pipe = HunyuanVideoPipeline.from_pretrained(
        model_id,
        transformer=transformer,
        torch_dtype=torch.float16,
    )
    print(f"  Pipeline loaded in {time.time()-t0:.1f}s")

    print("[3/3] Moving to GPU and enabling optimizations...")
    pipe.vae.enable_tiling()
    pipe.to("cuda:0")
    print("  Pipeline ready on GPU")

    return pipe


def render_scene(pipe, scene, output_dir, seed=42):
    """Render a single scene."""
    from diffusers.utils import export_to_video

    name = scene["name"]
    out_path = os.path.join(output_dir, f"{name}.mp4")

    if os.path.exists(out_path):
        print(f"  [skip] {out_path} already exists")
        return out_path

    print(f"  Prompt: {scene['prompt'][:80]}...")
    print(f"  Frames: {scene['num_frames']}, Guidance: {scene['guidance_scale']}")

    t0 = time.time()
    generator = torch.Generator(device="cuda").manual_seed(seed)

    result = pipe(
        prompt=scene["prompt"],
        height=720,
        width=1280,
        num_frames=scene["num_frames"],
        num_inference_steps=30,
        guidance_scale=scene["guidance_scale"],
        generator=generator,
    )

    video = result.frames[0]
    export_to_video(video, out_path, fps=24)

    elapsed = time.time() - t0
    size_mb = os.path.getsize(out_path) / 1024 / 1024
    print(f"  Done in {elapsed:.1f}s -> {out_path} ({size_mb:.1f} MB)")

    del result
    gc.collect()
    torch.cuda.empty_cache()

    return out_path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", default="studio/output/video_1/renders")
    parser.add_argument("--scene", type=int, default=0, help="1-4 or 0=all")
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    print("=" * 60)
    print("  SwarmRT Video Studio - HunyuanVideo 13B Renderer")
    print("=" * 60)
    print(f"  GPUs: {torch.cuda.device_count()}")
    for i in range(torch.cuda.device_count()):
        p = torch.cuda.get_device_properties(i)
        print(f"    GPU {i}: {p.name} ({p.total_memory/1e9:.0f} GB)")
    print()

    pipe = load_pipeline()

    scenes = SCENES if args.scene == 0 else [SCENES[args.scene - 1]]
    print(f"\nRendering {len(scenes)} scene(s)...\n")

    outputs = []
    for i, scene in enumerate(scenes):
        print(f"--- Scene {i+1}/{len(scenes)}: {scene['name']} ---")
        out = render_scene(pipe, scene, args.output_dir, seed=args.seed)
        outputs.append(out)
        print()

    print("=" * 60)
    print("  RENDER COMPLETE")
    print("=" * 60)
    for p in outputs:
        if os.path.exists(p):
            sz = os.path.getsize(p) / 1024 / 1024
            print(f"  {p} ({sz:.1f} MB)")


if __name__ == "__main__":
    main()
