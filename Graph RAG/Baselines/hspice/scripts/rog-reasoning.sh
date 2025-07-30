SPLIT="test"
DATASET_LIST="vanilla-hop2"
MODEL_NAME=RoG
PROMPT_PATH=prompts/llama2_predict.txt
BEAM_LIST="3" # "1 2 3 4 5"
TH_LIST="0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 "

for DATA_NAME in $DATASET_LIST; do
    for N_BEAM in $BEAM_LIST; do
        for threshold in $TH_LIST; do
            echo "$threshold">>result.txt
            RULE_PATH=results/gen_rule_path/${DATA_NAME}/${MODEL_NAME}/test/predictions_${N_BEAM}_False.jsonl
            python src/qa_prediction/predict_answer.py \
                --model_name ${MODEL_NAME} \
                -d "${DATA_NAME}" \
                --prompt_path ${PROMPT_PATH} \
                --add_rule \
                --rule_path "${RULE_PATH}" \
                --model_path rmanluo/RoG \
                --threshold "${threshold}"
            cat results/KGQA/vanilla-hop2/RoG/test/results_gen_rule_path_vanilla-hop2_RoG_test_predictions_3_False_jsonl/eval_result.txt>>result.txt
            printf "\n" > result.txt
            rm -r results/KGQA
        done
    done
done