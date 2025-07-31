# VLDB 2026 Artifact - SHARP
## Artifact for the VLDB 2026 Submission - *Sharp: Shared State Reduction for Efficient Matching of Sequential Patterns*

This repository provides the artifact for **Sharp**, a system for efficient best-effort pattern matching using shared state reduction. It supports three workloads: `CEP/` for Complex Event Processing, `MATCH_RECOGNIZE/` for SQL-based row pattern matching, and `GraphRAG/` for path-based pattern matching over knowledge graphs. Sharp leverages pattern-sharing and a lightweight cost model to significantly reduce computational overhead while preserving high recall under latency constraints.

## CEP and `MATCH_RECOGNIZE` Experiments

The codebase has been tested on Ubuntu 22.04, SUSE Linux Enterprise Server 15 SP5, and Red Hat Enterprise 9.5. For both `CEP/` and `MATCH_RECOGNIZE/`, enter each directory and install the necessary dependencies listed in `build_support/packages.sh`:

```sh
$ sudo build_support/packages.sh
$ sudo apt install libboost-all-dev
```

### Download Data

Download the required datasets from [synthetic dataset](https://drive.google.com/drive/folders/1_9XkUkKfz2OJObpYmy2pFOKn-DqcAKlu?usp=sharing), [real-world datasets](https://drive.google.com/drive/folders/13musleLNDuRnVAJNnCh4PoArSJB8nP_W?usp=sharing).  
Create the following directories and unzip the datasets inside accordingly:

```sh
$ mkdir synthetic_data real_data
# unzip downloaded files into the above folders
```

### Compile the Code

Inside both `CEP/` and `MATCH_RECOGNIZE/`:

```sh
$ mkdir build && cd build
$ cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=YES -DCMAKE_BUILD_TYPE=Debug ..
$ make -j$(nproc)
```

### Run Experiments

Navigate to the `scripts/` folder, set up a Python virtual environment, and install dependencies:

```sh
$ python -m venv venv && source venv/bin/activate
$ pip install pandas matplotlib
$ python recall_latency_throughput_parallel.py
```

---

## GraphRAG Experiment

Navigate to `GraphRAG/GraphRAG-SHARP/`, create a virtual environment, and install dependencies:

```sh
$ python -m venv venv && source venv/bin/activate
$ pip install -r requirements.txt
```

### Download Data

The dataset can be downloaded [here](https://github.com/yuyuz/MetaQA). 

### Inference Pipeline

Ensure access to a GPU (≥12GB). Set your Hugging Face token:

```sh
$ export HF_TOKEN="<TOKEN>"
```

#### Step 1: Generate Path Queries

```sh
$ ./scripts/planning.sh
```

#### Step 2: Generate Answers with LLM

After step 1 completes:

```sh
$ ./scripts/reasoning.sh
```

Repeat this process for each baseline folder.
