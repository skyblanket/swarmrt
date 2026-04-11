#!/usr/bin/env python3
"""
Load LTX-2 19B model from local files and render scenes.
Uses existing local model files + diffusers from_single_file.
"""

import argparse
import os
import sys
import time
import gc
import torch

# Scene prompts — concrete, filmable descriptions. No abstract concepts.
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
        "negative": "blurry, low quality, watermark, text, words, letters, caption, subtitle, grainy, distorted, cartoon, anime, bright, overexposed, ugly",
        "num_frames": 97,
        "guidance_scale": 7.0,
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
        "negative": "blurry, low quality, watermark, text, words, letters, caption, subtitle, distorted, cartoon, anime, people, faces, bright daylight",
        "num_frames": 97,
        "guidance_scale": 7.0,
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
        "negative": "blurry, low quality, watermark, text, words, letters, caption, subtitle, distorted, cartoon, anime, people, faces",
        "num_frames": 81,
        "guidance_scale": 7.0,
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
        "negative": "blurry, low quality, watermark, text, words, letters, caption, subtitle, distorted, cartoon, anime, daytime, clouds covering everything",
        "num_frames": 97,
        "guidance_scale": 7.0,
    },
]


def load_pipeline():
    """Load LTX-2 pipeline: text_encoder+vae from local, rest from HuggingFace."""
    from transformers import Gemma3ForConditionalGeneration, AutoTokenizer
    from diffusers import LTX2Pipeline, AutoencoderKLLTX2Video

    model_root = "/home/sky/LTX-2"
    hf_model_id = "Lightricks/LTX-2"

    # Load components from local disk (saves ~50GB download)
    print("[1/3] Loading text encoder (Gemma 3) from local...")
    t0 = time.time()
    text_encoder = Gemma3ForConditionalGeneration.from_pretrained(
        os.path.join(model_root, "text_encoder"),
        torch_dtype=torch.bfloat16,
    )
    print(f"  Loaded in {time.time()-t0:.1f}s")

    print("[2/3] Loading tokenizer + VAE from local...")
    tokenizer = AutoTokenizer.from_pretrained(
        os.path.join(model_root, "tokenizer"),
    )
    vae = AutoencoderKLLTX2Video.from_pretrained(
        os.path.join(model_root, "vae"),
        torch_dtype=torch.bfloat16,
    )
    print(f"  Tokenizer: {type(tokenizer).__name__}, VAE loaded")

    # Load the full pipeline from HuggingFace, passing local components
    # This will download only the transformer, scheduler, connectors, vocoder
    print("[3/3] Loading pipeline from HuggingFace (transformer + connectors)...")
    print("  This may download ~40GB on first run...")
    t0 = time.time()
    pipe = LTX2Pipeline.from_pretrained(
        hf_model_id,
        text_encoder=text_encoder,
        tokenizer=tokenizer,
        vae=vae,
        torch_dtype=torch.bfloat16,
    )
    print(f"  Pipeline loaded in {time.time()-t0:.1f}s")

    # Move to GPU - with 102GB VRAM, everything fits
    print("Moving to CUDA...")
    pipe.to("cuda:0")
    print("Pipeline ready on GPU")

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
        negative_prompt=scene["negative"],
        num_frames=scene["num_frames"],
        width=768,
        height=512,
        num_inference_steps=50,
        guidance_scale=scene["guidance_scale"],
        generator=generator,
    )

    video = result.frames[0]
    export_to_video(video, out_path, fps=24)

    elapsed = time.time() - t0
    size_mb = os.path.getsize(out_path) / 1024 / 1024
    print(f"  Done in {elapsed:.1f}s -> {out_path} ({size_mb:.1f} MB)")

    # Free memory
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
    print("  SwarmRT Video Studio - LTX-2 19B Renderer")
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
