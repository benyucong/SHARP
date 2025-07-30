import subprocess
import os
from concurrent.futures import ThreadPoolExecutor

os.chdir("../build/bin")

executable_path = "./cep_demo"

# Specify the path to the output file and folder
output_folder = "../../output/"
output_file = "output_recall_latency.csv"
output_path = os.path.join(output_folder, output_file)

# Ensure the output folder exists
os.makedirs(output_folder, exist_ok=True)

# Open the output file in append mode
output_file_mode = "a"

inf = 18446744073709551615
window = '1000'
ratio = '70'
header = 'Plan,Query,q0_bound,q1_bound,Shedding_method,q0_window,q1_window,shedding_ratio,q0_recall,q1_recall,shed_partial_match,shed_event,throughput'
# process = subprocess.Popen([executable_path] + ['2', str(inf), str(inf), '1',
#                                                 window, window, ratio], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
# stdout, stderr = process.communicate()
# q0_full_detection, q1_full_detection, throughput = stdout.decode().split(',')

q0_full_detection = float(3791)
q1_full_detection = float(6076)
throughput = float(21421.6)
print(f"Full detections done! They are {q0_full_detection} and {q1_full_detection}.")
print(f"Throughput without shedding is {throughput}")

with open(output_path, output_file_mode) as file:
    if output_file_mode == "a":
        file.write(header + "\n")
        output_file_mode = "a+"


def run_program(plan_choice, q0_bound, q1_bound, shed_method, window, executable_path, output_path, output_file_mode):
    parameters = [str(plan_choice), str(q0_bound), str(
        q1_bound), str(shed_method), window, window, ratio]
    process = subprocess.Popen([executable_path] + parameters, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stdout, stderr = process.communicate()

    result_list = stdout.decode().split(',')
    result_list[8] = str(round(float(result_list[8]) * 100 / q0_full_detection, 1))
    result_list[9] = str(round(float(result_list[9]) * 100 / q1_full_detection, 1))
    new_result = ','.join(result_list)
    with open(output_path, output_file_mode) as file:
        file.write(new_result + "\n")


def main():
    """ plan_choice: 0 for static shared; 1 for static non-shared; 2 for dynamic shared;
    shed_method, the load shedding strategy we choose:
        0 -> Cost-Model Shedding
        1 -> Random State Shedding
        2 -> Fractional Load Shedding
        3 -> DARLING
        4 -> ICDE'20
        5 -> gspice
        6 -> hspice
    """
    plan_choices = [0, 1, 2]
    shed_methods = [4, 1, 2, 3, 0, 5, 6]
    # shed_methods = [4, 1, 0]

    scan_start = 9000
    scan_end = scan_start * 20
    scan_step = scan_start

    # Adjust max_workers as needed
    with ThreadPoolExecutor(max_workers=8) as executor:
        for q0_bound, q1_bound in zip(range(scan_start, scan_end, scan_step), range(scan_start, scan_end, scan_step)):
            for plan_choice in plan_choices:
                for shed_method in shed_methods:
                    executor.submit(run_program, plan_choice, q0_bound, q1_bound, shed_method, window, executable_path,
                                    output_path, output_file_mode)


if __name__ == "__main__":
    main()
    print(f"Output saved to {output_path}")
