# Copyright (C) 2024 Intel Corporation
# Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.TXT
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import collections, re
from benches.result import Result
from options import options
import math
import ast

class OutputLine:
    def __init__(self, name):
        self.label = name
        self.diff = None
        self.bars = None
        self.row = ""
        self.suite = "Unknown"
        self.explicit_group = ""

    def __str__(self):
        return f"(Label:{self.label}, diff:{self.diff})"

    def __repr__(self):
        return self.__str__()

# Independent of the chart_data content number of required columns in the markdown table:
# Benchmark_name
#
# optional +1: relative performance value in given units
num_info_columns = 1

# Number of columns required for relative performance change calculation
# In case of multiple provided saved baseline to compare, the relative
# performance is not calculated, since the base (hopefully) usage case 
# for this script would be comparing the performance of PR with the main branch
num_baselines_required_for_rel_change = 2

# Maximum number of characters that is allowed in request validation
# for posting comments in GitHub PR
max_markdown_size = 65536

def is_relative_perf_comparison_to_be_performed(chart_data: dict[str, list[Result]], baseline_name: str):
    return (len(chart_data) == num_baselines_required_for_rel_change) and(baseline_name in chart_data.keys())
    

def get_chart_markdown_header(chart_data: dict[str, list[Result]], baseline_name: str):
    summary_header = ''
    final_num_columns = num_info_columns

    if is_relative_perf_comparison_to_be_performed(chart_data, baseline_name):
        summary_header = "| Benchmark | " + " | ".join(chart_data.keys()) + " | Change |\n"
        final_num_columns += 1
    else:
        summary_header = "| Benchmark | " + " | ".join(chart_data.keys()) + " |\n"

    summary_header += "|---" * (len(chart_data) + final_num_columns) + "|\n"

    return summary_header


def get_main_branch_run_name(chart_data: dict[str, list[Result]], baseline_name: str):
    for key in chart_data.keys():
        if key != baseline_name:
            return key
        
    return None

def get_available_markdown_size(current_markdown_size: int):
    return max(0, max_markdown_size - current_markdown_size)

def is_content_in_size_limit(content_size: int, current_markdown_size: int):
    return content_size <= get_available_markdown_size(current_markdown_size)

# Function to generate the markdown collapsible sections for each variant
def generate_markdown_details(results: list[Result], current_markdown_size: int, is_output_always_full: bool):
    markdown_sections = []
    # print("results all", len(results))
    # print("first res", results[0])
    # print("res keys", results.keys())
    markdown_sections.append(f"""
<details>
<summary>Benchmark details - environment, command...</summary>
""")

    for res in results:
        
        # print("res")
        # print("res env", res.env)
        env_dict = res.env
        command = res.command

        if isinstance(res.env, str):
            env_dict = ast.literal_eval(res.env)
        if isinstance(res.command, str):
            command = ast.literal_eval(res.command)

        env_vars_str = '\n'.join(f"{key}={value}" for key, value in env_dict.items()) # res.env.items())
        section = f"""
<details>
<summary>{res.label}</summary>

#### Command:
{' '.join(command)}

"""
        if env_dict:
            section += f"""
#### Environment Variables:
{env_vars_str}

"""
        section += f"""
</details>
"""
            
        markdown_sections.append(section)

    markdown_sections.append(f"""
</details>
""")
    
    full_markdown = "\n".join(markdown_sections)

    if is_output_always_full:
        return full_markdown
    else:
        if is_content_in_size_limit(len(full_markdown), current_markdown_size):
            return full_markdown
        else:
            return "\nBenchmark details contain too many chars to display"

def generate_summary_table_and_chart(chart_data: dict[str, list[Result]], baseline_name: str, is_output_always_full: bool):
    summary_table = get_chart_markdown_header(chart_data=chart_data, baseline_name=baseline_name) #"| Benchmark | " + " | ".join(chart_data.keys()) + " | Relative perf | Change |\n"
    # summary_table += "|---" * (len(chart_data) + 2) + "|\n"
    print("len chart data", len(chart_data))
    print("chart keys", chart_data.keys())

    # Collect all benchmarks and their results
    # key: benchmark name, value: dict(run_name : single_result_in_the_given_run)
    benchmark_results = collections.defaultdict(dict)
    debug = None
    # key: run name, results: results from different benchmarks collected in the named run
    for key, results in chart_data.items():
        # print("k", key, "r", results)
        for res in results:
            # print("res", res)
            debug = res.name
            benchmark_results[res.name][key] = res

    print("ben len", len(benchmark_results))
    # print(benchmark_results.keys(), "\n", len(benchmark_results[debug]),  "\n", benchmark_results[debug], "\n", benchmark_results[debug]["baseline"])
    # Generate the table rows
    output_detailed_list = []


    # global_product = 1
    # mean_cnt = 0
    # improved = 0
    # regressed = 0
    no_change = 0

    # suite_dicts = collections.defaultdict(list)

    for bname, results in benchmark_results.items():
        oln = OutputLine(bname)
        oln.row = f"| {bname} |"
        best_value = None
        best_key = None

        are_suite_group_assigned = False

        # Determine the best value for the given benchmark, among the results from all saved runs specified by --compare
        # key: run name, res: single result collected in the given run
        for key, res in results.items():
            if not are_suite_group_assigned:
                oln.suite = res.suite
                oln.explicit_group = res.explicit_group

                are_suite_group_assigned = True

            if best_value is None or (res.lower_is_better and res.value < best_value) or (not res.lower_is_better and res.value > best_value):
                best_value = res.value
                best_key = key

        # Generate the row with all the results from saved runs specified by --compare,
        # Highight the best value in the row with data
        if options.verbose: print(f"Results: {results}")
        for key in chart_data.keys():
            if key in results:
                intv = results[key].value
                if key == best_key:
                    oln.row += f" <ins>{intv:3f}</ins> {results[key].unit} |"  # Highlight the best value
                else:
                    oln.row += f" {intv:.3f} {results[key].unit} |"
            else:
                oln.row += " - |"

        # print("keys", chart_data.keys())
        # key0 = list(chart_data.keys())[0]
        # key1 = list(chart_data.keys())[1]
        # print("k0 in results", key0 in results, "k1 in results", key1 in results)
        if is_relative_perf_comparison_to_be_performed(chart_data, baseline_name):
            # pr_key = list(chart_data.keys())[0]
            pr_key = baseline_name
            main_key = get_main_branch_run_name(chart_data, baseline_name) #list(chart_data.keys())[1]
            # print("k0 in results", key0 in results, "k1 in results", key1 in results)
            if (pr_key in results) and (main_key in results):
                pr_val = results[pr_key].value
                main_val = results[main_key].value
                diff = None
                # print("v0", v0, "v1", v1)
                if pr_val != 0 and results[pr_key].lower_is_better:
                    diff = main_val / pr_val
                elif main_val != 0 and not results[pr_key].lower_is_better:
                    diff = pr_val / main_val

                if diff != None:
                    # oln.row += f" {(diff * 100):.2f}%"
                    oln.diff = diff

        # if representative is not None:
        #     oln.suite = representative.s

        output_detailed_list.append(oln)


    sorted_detailed_list = sorted(output_detailed_list, key=lambda x: (x.diff is not None, x.diff), reverse=True)

    diffs_sorted = [x.diff for x in sorted_detailed_list]
    print(diffs_sorted)

    print("slen", len(sorted_detailed_list)) #, "\n", sorted_detailed_list)

    diff_values = [oln.diff for oln in sorted_detailed_list if oln.diff is not None]
    # print("oln", oln, "\n")
    # print("diffs:", diff_values)

    improved_rows = []
    regressed_rows = []
    print("diff values len", len(diff_values))
    if len(diff_values) > 0:
        # max_diff = max(max(diff_values) - 1, 1 - min(diff_values))

        for oln in sorted_detailed_list:
            if oln.diff != None:
                # print("Not none diff")
                oln.row += f" {(oln.diff - 1)*100:.2f}%"
                delta = oln.diff - 1
                # oln.bars = round(10*(oln.diff - 1)/max_diff) if max_diff != 0.0 else 0
                # if oln.bars == 0 or abs(delta) < options.epsilon:
                #     oln.row += " | . |"
                # elif oln.bars > 0:
                #     oln.row += f" | {'+' * oln.bars} |"
                # else:
                #     oln.row += f" | {'-' * (-oln.bars)} |"

                # mean_cnt += 1
                # is_at_least_one_diff = True
                if abs(delta) > options.epsilon:
                    if delta > 0:
                        # improved+=1
                        improved_rows.append(oln.row + " | \n")
                    else:
                        # regressed+=1
                        regressed_rows.append(oln.row + " | \n")
                else:
                    no_change+=1

                # global_product *= oln.diff
            # else:
            #     # print("diff is None for", oln.row)
            #     oln.row += "   |"

            if options.verbose: print(oln.row)
            summary_table += oln.row + "\n"
    else:
        for oln in sorted_detailed_list:
            # oln.row += "  |"
            # if options.verbose: print(oln.row)
            summary_table += oln.row + "\n"

    regressed_rows.reverse()

    # grouped_objects = dict(grouped_objects)

    # print("mean:", mean_cnt)
    # if mean_cnt > 0:
    is_at_least_one_diff = False
        # global_mean = global_product ** (1/mean_cnt)
        # summary_line = f"Total {mean_cnt} benchmarks in mean. "
        # summary_line += "\n" + f"Geomean {global_mean*100:.3f}%. \nImproved {improved} Regressed {regressed} (threshold {options.epsilon*100:.2f}%)"
        # print("enter summary line")
    summary_line = '' #'\n'
    
    if len(improved_rows) > 0:
        is_at_least_one_diff = True
        summary_line += f"""
<details>
<summary>        
Improved {len(improved_rows)} (threshold {options.epsilon*100:.2f}%) 
</summary>

"""
        summary_line += get_chart_markdown_header(chart_data=chart_data, baseline_name=baseline_name) 
        #"\n\n| Benchmark | " + " | ".join(chart_data.keys()) + " | Relative perf | Change |\n"
        # summary_line += "|---" * (len(chart_data) + 4) + "|\n"

        for row in improved_rows:
            summary_line += row #+ "\n"

        summary_line += "\n</details>"
    
    if len(regressed_rows) > 0:
        is_at_least_one_diff = True
        summary_line += f"""
<details>
<summary>        
Regressed {len(regressed_rows)} (threshold {options.epsilon*100:.2f}%) </summary>

"""
    
        summary_line += get_chart_markdown_header(chart_data=chart_data, baseline_name=baseline_name) 
        #"\n\n| Benchmark | " + " | ".join(chart_data.keys()) + " | Relative perf | Change |\n"
        # summary_line += "|---" * (len(chart_data) + 2) + "|\n"

        for row in regressed_rows:
            summary_line += row #+ "\n"
        
        summary_line += "\n</details>"

    if not is_at_least_one_diff:
        summary_line = f"No diffs to calculate performance change"

    if options.verbose: print(summary_line)


    summary_table = "\n## Performance change in benchmark groups\n"

    grouped_in_suites = collections.defaultdict(lambda: collections.defaultdict(list))
    for oln in output_detailed_list:
        # s = oln.label
        # prefix = re.match(r'^[^_\s]+', s)[0]
        grouped_in_suites[oln.suite][oln.explicit_group].append(oln)

    
    for suite_name, suite_groups in grouped_in_suites.items():
        summary_table += f"<details><summary>{suite_name}</summary>\n\n"

        for name, outgroup in suite_groups.items():
            outgroup_s = sorted(outgroup, key=lambda x: (x.diff is not None, x.diff), reverse=True)
            product = 1.0
            n = len(outgroup_s)
            r = 0
            for oln in outgroup_s:
                if oln.diff != None:
                    product *= oln.diff
                    r += 1
            if r > 0:
                summary_table += f"""
<details>
<summary> Relative perf in group {name} ({n}): {math.pow(product, 1/r)*100:.3f}% </summary>

"""
            else:
                summary_table += f"""
<details>
<summary> Relative perf in group {name} ({n}): cannot calculate </summary>

"""
            summary_table += get_chart_markdown_header(chart_data, baseline_name) #"| Benchmark | " + " | ".join(chart_data.keys()) + " | Relative perf | Change |\n"
            #summary_table += "|---" * (len(chart_data) + 3) + "|\n"

            for oln in outgroup_s:
                summary_table += f"{oln.row}\n"

            summary_table += f"""
</details>

"""
        summary_table += "</details>"

    if is_output_always_full:
        return summary_line, summary_table
    else:
        if is_content_in_size_limit(content_size=len(summary_table), current_markdown_size=len(summary_line)):
            return summary_line, summary_table
        else:
            if is_content_in_size_limit(content_size=len(summary_line), current_markdown_size=0):
                return summary_line
            else:
                return f"""
# Summary
Benchmark output is too large to display

"""

def generate_markdown(name: str, chart_data: dict[str, list[Result]], is_output_always_full: bool):
    (summary_line, summary_table) = generate_summary_table_and_chart(chart_data, name, is_output_always_full)

    current_markdown_size = len(summary_line) + len(summary_table)

    generated_markdown = f"""
# Summary
(<ins>Emphasized values</ins> are the best results)\n
{summary_line}\n
{summary_table}
"""
    if name in chart_data.keys():
        generated_markdown += f"""
# Details
{generate_markdown_details(chart_data[name], current_markdown_size, is_output_always_full)}
"""
    return generated_markdown
