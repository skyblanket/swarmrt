#!/usr/bin/env python3
"""
SwarmRT Video Studio — Scene Renderer
Uses diffusers LTXPipeline to render video scenes from generated prompts.

Usage: python render_scenes.py [--output-dir DIR] [--scene N]
"""

import argparse
import os
import time
import torch
from diffusers import LTXPipeline
from diffusers.utils import export_to_video

# Scene prompts from video_studio output
SCENES = [
    {
        "name": "scene_1_native_genesis",
        "prompt": (
            "Macro probe lens pressed against brushed aluminum matte surface "
            "with wet-look holographic iridescence, revealing silicon wafer atomic "
            "lattice structure under sharp volumetric amber edge lighting. "
            "Camera pulls back rapidly through hexagonal aperture into deep black void, "
            "revealing thousands of autonomous agents as dense starling murmuration "
            "rendered in purple and cyan particles swirling in impossible geometric "
            "patterns. Swarm crystallizes into glowing binary code streams that snap "
            "into solid silicon structures. Cinematic chiaroscuro, crystalline texture, "
            "kinetic motion."
        ),
        "negative": "blurry, low quality, watermark, text, words, letters, caption, subtitle, grainy, distorted, shaky camera, overexposed, cartoon, anime",
        "num_frames": 97,  # LTX works best with 8k+1 frames
        "guidance_scale": 7.5,
    },
    {
        "name": "scene_2_lockfree_velocity",
        "prompt": (
            "Extreme close-up of fiber-optic data trails pulsing cyan through translucent "
            "crystalline conduits at velocity-blur speeds. Camera orbits 180 degrees around "
            "iridescent purple polyhedral structure with amber glowing vertices. "
            "Larger rotating brushed aluminum shields with wet-look holographic surfaces "
            "reflecting data streams. Morphing code blocks phase-shifting through shields "
            "while maintaining unbroken light-threads. Purple orbs momentarily enclosed in "
            "cyan hexagonal cages that clean and release them. Sharp volumetric god rays "
            "cutting through crystalline density."
        ),
        "negative": "blurry, low quality, watermark, text, words, letters, caption, subtitle, grainy, distorted, shaky camera, overexposed, cartoon, anime",
        "num_frames": 97,
        "guidance_scale": 7.5,
    },
    {
        "name": "scene_3_handforged_metal",
        "prompt": (
            "Macro probe lens tracks slowly across raw assembly instructions etched in "
            "glowing amber into brushed aluminum matte surface, sparks flying as machine "
            "code physically carves into metal. Extreme close-up reveals hand-wrought "
            "silicon craftsmanship with wet-look holographic iridescence highlighting "
            "chisel marks. Sharp chiaroscuro lighting emphasizes crystalline texture of "
            "raw binary crystallizing into hardware structures. Camera tilts upward "
            "following metallic shavings floating upward against deep black background, "
            "transforming into autonomous agent particles. Zero abstraction visualization "
            "showing bare metal genesis with kinetic precision and dense mechanical detail."
        ),
        "negative": "blurry, low quality, watermark, text, words, letters, caption, subtitle, grainy, distorted, shaky camera, overexposed, cartoon, anime",
        "num_frames": 81,
        "guidance_scale": 7.5,
    },
    {
        "name": "scene_4_network_awakening",
        "prompt": (
            "Orbital drone swarm camera pulls back rapidly from individual crystalline "
            "nodes to reveal planetary-scale network architecture, millions of purple and "
            "cyan autonomous agents forming vast neural lattice across dark space. "
            "Bright binary pulses through fiber-optic highways connecting hand-forged amber "
            "silicon structures in impossible geometries. Volumetric edge lighting creates "
            "sharp chiaroscuro highlighting the emergent density of synchronized messaging. "
            "Crystalline conduits bloom with wet-look holographic iridescence as the swarm "
            "achieves consciousness, particles swirling in mathematically perfect murmuration "
            "patterns. Dense, kinetic, transcendent finale."
        ),
        "negative": "blurry, low quality, watermark, text, words, letters, caption, subtitle, grainy, distorted, shaky camera, overexposed, cartoon, anime",
        "num_frames": 97,
        "guidance_scale": 7.5,
    },
]


def render_scene(pipe, scene, output_dir, seed=42):
    """Render a single scene and save as mp4."""
    name = scene["name"]
    out_path = os.path.join(output_dir, f"{name}.mp4")

    if os.path.exists(out_path):
        print(f"  [skip] {out_path} already exists")
        return out_path

    print(f"  Generating {name} ({scene['num_frames']} frames)...")
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
    print(f"  [{name}] done in {elapsed:.1f}s → {out_path}")
    return out_path


def main():
    parser = argparse.ArgumentParser(description="Render video scenes")
    parser.add_argument("--output-dir", default="studio/output/video_1/renders",
                        help="Output directory for rendered videos")
    parser.add_argument("--scene", type=int, default=0,
                        help="Render specific scene (1-4), 0=all")
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    print("=" * 60)
    print("  SwarmRT Video Studio — Scene Renderer")
    print("=" * 60)
    print()

    # Load pipeline
    print("[1/2] Loading LTX-Video pipeline...")
    t0 = time.time()

    pipe = LTXPipeline.from_pretrained(
        "Lightricks/LTX-Video",
        torch_dtype=torch.bfloat16,
    )
    pipe.to("cuda")

    # Enable memory-efficient attention
    pipe.enable_model_cpu_offload()

    elapsed = time.time() - t0
    print(f"  Pipeline loaded in {elapsed:.1f}s")
    print()

    # Render scenes
    scenes_to_render = SCENES if args.scene == 0 else [SCENES[args.scene - 1]]
    total = len(scenes_to_render)

    print(f"[2/2] Rendering {total} scene(s)...")
    print()

    outputs = []
    for i, scene in enumerate(scenes_to_render):
        print(f"--- Scene {i+1}/{total}: {scene['name']} ---")
        out = render_scene(pipe, scene, args.output_dir, seed=args.seed)
        outputs.append(out)
        print()

    print("=" * 60)
    print("  RENDER COMPLETE")
    print("=" * 60)
    for p in outputs:
        sz = os.path.getsize(p) / 1024 / 1024 if os.path.exists(p) else 0
        print(f"  {p} ({sz:.1f} MB)")
    print()


if __name__ == "__main__":
    main()
