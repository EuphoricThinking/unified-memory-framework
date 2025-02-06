# Copyright (C) 2024 Intel Corporation
# Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
# See LICENSE.TXT
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

import collections, re
from benches.result import Result
from options import options
import math

class OutputLine:
    def __init__(self, name):
        self.label = name
        self.diff = None
        self.bars = None
        self.row = ""

    def __str__(self):
        return f"(Label:{self.label}, diff:{self.diff})"

    def __repr__(self):
        return self.__str__()

# Benchmark_name, relative performance value in given units
num_info_columns = 3
    
def get_chart_markdown_header(chart_data: dict[str, list[Result]]):
    summary_header = "| Benchmark | " + " | ".join(chart_data.keys()) + " | Relative perf | Change |\n"
    summary_header += "|---" * (len(chart_data) + num_info_columns) + "|\n"

    return summary_header



# Function to generate the markdown collapsible sections for each variant
def generate_markdown_details(results: list[Result]):
    markdown_sections = []

    markdown_sections.append(f"""
<details>
<summary>Benchmark details - environment, command...</summary>
""")

    for res in results:
        env_vars_str = '\n'.join(f"{key}={value}" for key, value in res.env.items())
        markdown_sections.append(f"""
<details>
<summary>{res.label}</summary>

#### Environment Variables:
{env_vars_str}

#### Command:
{' '.join(res.command)}

</details>
""")
    markdown_sections.append(f"""
</details>
""")
    return "\n".join(markdown_sections)

def generate_summary_table_and_chart(chart_data: dict[str, list[Result]]):
    summary_table = get_chart_markdown_header(chart_data=chart_data) #"| Benchmark | " + " | ".join(chart_data.keys()) + " | Relative perf | Change |\n"
    # summary_table += "|---" * (len(chart_data) + 2) + "|\n"
    print("len chart data", len(chart_data))
    print("chart keys", chart_data.keys())

    # Collect all benchmarks and their results
    benchmark_results = collections.defaultdict(dict)
    for key, results in chart_data.items():
        # print("k", key, "r", results)
        for res in results:
            # print("res", res)
            benchmark_results[res.name][key] = res

    print("ben len", len(benchmark_results))
    # Generate the table rows
    output_detailed_list = []


    # global_product = 1
    # mean_cnt = 0
    # improved = 0
    # regressed = 0
    no_change = 0

    for bname, results in benchmark_results.items():
        oln = OutputLine(bname)
        oln.row = f"| {bname} |"
        best_value = None
        best_key = None

        # Determine the best value for the given benchmark, among the results from all saved runs specified by --compare
        for key, res in results.items():
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
        if len(chart_data.keys()) == 2:
            key0 = list(chart_data.keys())[0]
            key1 = list(chart_data.keys())[1]
            print("k0 in results", key0 in results, "k1 in results", key1 in results)
            if (key0 in results) and (key1 in results):
                v0 = results[key0].value
                v1 = results[key1].value
                diff = None
                print("v0", v0, "v1", v1)
                if v0 != 0 and results[key0].lower_is_better:
                    diff = v1/v0
                elif v1 != 0 and not results[key0].lower_is_better:
                    diff = v0/v1

                if diff != None:
                    oln.row += f" LALAAA {(diff * 100):.2f}%"
                    oln.diff = diff

        output_detailed_list.append(oln)


    sorted_detailed_list = sorted(output_detailed_list, key=lambda x: (x.diff is not None, x.diff), reverse=True)

    diffs_sorted = [x.diff for x in sorted_detailed_list]
    print(diffs_sorted)

    print("slen", len(sorted_detailed_list)) #, "\n", sorted_detailed_list)

    diff_values = [oln.diff for oln in sorted_detailed_list if oln.diff is not None]
    # print("oln", oln, "\n")
    print("diffs:", diff_values)

    improved_rows = []
    regressed_rows = []
    print("diff values len", len(diff_values))
    if len(diff_values) > 0:
        # max_diff = max(max(diff_values) - 1, 1 - min(diff_values))

        for oln in sorted_detailed_list:
            if oln.diff != None:
                # print("Not none diff")
                oln.row += f" | {(oln.diff - 1)*100:.2f}%"
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
            else:
                # print("diff is None for", oln.row)
                oln.row += " |   |"

            if options.verbose: print(oln.row)
            summary_table += oln.row + "\n"
    else:
        for oln in sorted_detailed_list:
            oln.row += " |   |"
            if options.verbose: print(oln.row)
            summary_table += oln.row + "\n"

    grouped_objects = collections.defaultdict(list)
    regressed_rows.reverse()

    for oln in output_detailed_list:
        s = oln.label
        prefix = re.match(r'^[^_\s]+', s)[0]
        grouped_objects[prefix].append(oln)

    grouped_objects = dict(grouped_objects)

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
        summary_line += get_chart_markdown_header(chart_data=chart_data) 
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
    
        summary_line += get_chart_markdown_header(chart_data=chart_data) 
        #"\n\n| Benchmark | " + " | ".join(chart_data.keys()) + " | Relative perf | Change |\n"
        # summary_line += "|---" * (len(chart_data) + 2) + "|\n"

        for row in regressed_rows:
            summary_line += row #+ "\n"
        
        summary_line += "\n</details>"

    if not is_at_least_one_diff:
        summary_line = f"No diffs to calculate performance change"

    if options.verbose: print(summary_line)


    summary_table = "\n## Performance change in benchmark groups\n"

    for name, outgroup in grouped_objects.items():
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
        summary_table += "| Benchmark | " + " | ".join(chart_data.keys()) + " | Relative perf | Change |\n"
        summary_table += "|---" * (len(chart_data) + 3) + "|\n"

        for oln in outgroup_s:
            summary_table += f"{oln.row}\n"

        summary_table += f"""
</details>

"""

    return summary_line, summary_table

def generate_markdown(name: str, chart_data: dict[str, list[Result]]):
    (summary_line, summary_table) = generate_summary_table_and_chart(chart_data)

    generated_markdown = f"""
# Summary
{summary_line}\n
(<ins>result</ins> is better)\n
{summary_table}
"""
    if name in chart_data.keys():
        generated_markdown += f"""
# Details
{generate_markdown_details(chart_data[name])}
"""
    return generated_markdown
