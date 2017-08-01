#!/usr/bin/python

import os, subprocess, re, sys, stat, pickle

check_run = True

#benchmark_list = ['mcf', 'milc', 'soplex', 'sphinx3']

running_script_base = """#!/bin/bash
#source /u/akanksha/.bashrc
/u/hejy/CompArch/yan-sniper/sniper/run-sniper -c {1} -d {0} --pinballs {2}
"""

pinball_base = "/scratch/cluster/haowu/smets/sniper_pinpoints/cpu2006_pinballs/{0}/pinball_short.pp/{1}"

condor_script_base = """
+Group="GRAD"
+Project="ARCHITECTURE"
+ProjectDescription="Architectural Simulation to study caches using sniper simulator"
universe=vanilla
coresize=0
getenv=true
Rank=Memory
notification=Error
input=
output={0}/CONDOR.OUT
error={0}/CONDOR.ERR
Log={0}/CONDOR.LOG
requirements = InMastodon && ((Narsil == True) || (Uvanimor == True))
initialdir = {0}
executable = {0}/run.sh
queue
"""

#pinpoint_dir = "/u/haowu/scratch/smets/sniper_pinpoints/cpu2006_pinballs"
output_dir_base = "/u/hejy/CompArch/yan-sniper/sniper/final-output/{0}/{1}"
#output_root_dir_base = "/u/hejy/yan-sniper/sniper/output/{0}"
config_file_base = "/u/hejy/CompArch/yan-sniper/sniper/run_config/4core_config/{0}.cfg"

pinballs_dir_base = "/u/haowu/scratch/smets/sniper_pinpoints/cpu2006_pinballs/{0}/pinball_short.pp"
pinballs_file_base = "/u/haowu/scratch/smets/sniper_pinpoints/cpu2006_pinballs/{0}/pinball_short.pp/{1}"


def get_pinpoint_set(benchmark):
    pinballs_dir = pinballs_dir_base.format(benchmark)
    pinpoint_set = {}

    for file in os.listdir(pinballs_dir):
        res = re.match("(pinball_t0r(\d+)_(.+)).address", file)
        if res != None:
            simpoint_no = int(res.group(2))
            pinball_name = res.group(1)
            print simpoint_no, pinball_name
            pinpoint_set[simpoint_no] = pinball_name

    return pinpoint_set

def execute(condor_file):
    subprocess.call(['/lusr/opt/condor/bin/condor_submit', condor_file])

def getOutputDir(config_name, index):
    output_dir = output_dir_base.format(config_name, index)
    return output_dir

def getRunningScript(output_dir, config_file, pinpoint_dict, benchmark_list):
    pinpoint_text = ""
    for benchmark in benchmark_list:
        pinpoint = pinpoint_dict[benchmark]
        pinpoint_text += pinball_base.format(benchmark, pinpoint)
        pinpoint_text += ','
    pinpoint_text = pinpoint_text[:-1]
#    print pinpoint_text

    running_script = running_script_base.format(output_dir, config_file,
            pinpoint_text)

    return running_script



def createScripts(config_name, index, benchmark_list):
    pinpoint_dict = {}

    for benchmark in benchmark_list:
        pinpoint_set = get_pinpoint_set(benchmark)
        pinpoint_dict[benchmark] = pinpoint_set[1]
    
    #print pinpoint_dict

    output_dir = getOutputDir(config_name, index)
    print output_dir

    if not os.path.exists(output_dir):
        os.mkdir(output_dir)

    config_file = config_file_base.format(sys.argv[3])
    condor_script = condor_script_base.format(output_dir)

    running_script = getRunningScript(output_dir, config_file, pinpoint_dict, benchmark_list)

    condor_script_filename = "{0}/run.condor".format(output_dir)
    running_script_filename = "{0}/run.sh".format(output_dir)

    with open(condor_script_filename, "w") as condor_script_file:
        condor_script_file.write(condor_script)

    with open(running_script_filename, "w") as running_script_file:
        running_script_file.write(running_script)

    os.chmod(running_script_filename, stat.S_IRWXU)

    execute(condor_script_filename)


if __name__ == "__main__":

    if len(sys.argv) < 4:
        print "Usage: run_all.py benchmark_list output_dir config_file"
        exit(-1)

    output_root_dir = output_dir_base.format(sys.argv[2], '', '')
    if not os.path.exists(output_root_dir):
        os.mkdir(output_root_dir)

    with open(sys.argv[1]) as benchmark_list_file:
        benchmark_list = pickle.load(benchmark_list_file)

    for index, benchmarks in enumerate(benchmark_list):
        assert(len(benchmarks) == 4)
        createScripts(sys.argv[2], index, benchmarks)



