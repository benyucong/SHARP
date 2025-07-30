#!/bin/bash
#SBATCH --time=5-00:00:00
#SBATCH --cpus-per-task=40
#SBATCH --partition=gpu-a100-80g,gpu-h100-80g
#SBATCH --mem=100G
#SBATCH --gres=gpu:1
#SBATCH --constraint="a100|h100"

ml cuda


export HF_HOME="${WRKDIR}/hf_cache"
export HF_TOKEN=""
./scripts/rog-reasoning.sh
