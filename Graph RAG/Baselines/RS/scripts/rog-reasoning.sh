SPLIT="test"
DATASET_LIST="vanilla-hop2"
MODEL_NAME=RoG
PROMPT_PATH=prompts/llama2_predict.txt
BEAM_LIST="3" # "1 2 3 4 5"

for DATA_NAME in $DATASET_LIST; do
    for N_BEAM in $BEAM_LIST; do
        for queue_len in {1..15}; do
            echo "$queue_len">>result.txt
            RULE_PATH=results/gen_rule_path/${DATA_NAME}/${MODEL_NAME}/test/predictions_${N_BEAM}_False.jsonl
            python src/qa_prediction/predict_answer.py \
                --model_name ${MODEL_NAME} \
                -d "${DATA_NAME}" \
                --prompt_path ${PROMPT_PATH} \
                --add_rule \
                --rule_path "${RULE_PATH}" \
                --model_path rmanluo/RoG \
                --queue_len "${queue_len}"
            cat results/KGQA/vanilla-hop2/RoG/test/results_gen_rule_path_vanilla-hop2_RoG_test_predictions_3_False_jsonl/eval_result.txt>>result.txt
            rm -r results/KGQA
        done
    done
done