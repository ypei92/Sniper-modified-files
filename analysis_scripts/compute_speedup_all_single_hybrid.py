#!/usr/bin/python

import os, subprocess, re, sys, stat, sniper_lib


#benchmark_set = ['astar', 'bwaves', 'bzip2', 'cactusADM', 'calculix', 'gamess', 'gcc', 'GemsFDTD', 'gobmk', 'gromacs', 'h264ref', 'hmmer', 'lbm', 'leslie3d', 'libquantum', 'mcf', 'milc', 'omnetpp', 'perlbench', 'sjeng', 'soplex', 'sphinx3', 'tonto', 'wrf', 'xalancbmk', 'zeusmp']
#benchmark_set = ['astar', 'bzip2', 'cactusADM', 'calculix', 'gcc', 'GemsFDTD', 'gobmk', 'gromacs', 'h264ref', 'hmmer', 'lbm', 'leslie3d', 'libquantum', 'mcf', 'soplex', 'sphinx3', 'tonto', 'xalancbmk', 'zeusmp', 'omnetpp']
benchmark_set = ['astar', 'cactusADM', 'mcf', 'soplex', 'sphinx3', 'xalancbmk']
#Irregular
#benchmark_set = ['bzip2', 'gcc', 'mcf', 'omnetpp', 'perlbench', 'soplex', 'sphinx3', 'xalancbmk', 'gobmk', 'hmmer', 'h264ref', 'libquantum']
#Regular
#benchmark_set = ['bwaves', 'cactusADM', 'calculix', 'GemsFDTD', 'gromacs', 'lbm', 'leslie3d', 'milc', 'tonto', 'wrf', 'zeusmp', 'astar', 'gcc', 'libquantum', 'mcf', 'soplex', 'sphinx3', 'xalancbmk', 'omnetpp']
#benchmark_set = ['lbm', 'milc', 'astar', 'libquantum', 'mcf', 'soplex', 'xalancbmk', 'omnetpp', 'zeusmp']
#benchmark_set = ['bwaves', 'cactusADM', 'calculix', 'GemsFDTD', 'gromacs', 'lbm', 'leslie3d', 'milc', 'tonto', 'wrf', 'zeusmp']
#benchmark_set = ['astar', 'gcc', 'libquantum', 'mcf', 'soplex', 'sphinx3', 'xalancbmk', 'omnetpp']
#benchmark_set = ['milc']
#benchmark_set = ['cactusADM', 'lbm', 'leslie3d', 'milc', 'tonto', 'wrf', 'zeusmp', 'sphinx3', 'soplex']
#benchmark_set = ['bwaves', 'cactusADM', 'calculix', 'GemsFDTD', 'gromacs', 'lbm', 'leslie3d', 'milc', 'tonto', 'wrf', 'zeusmp', 'sphinx3', 'soplex']

# cs395t-lin
benchmark_set = ['astar', 'bzip2', 'gcc', 'libquantum', 'mcf', 'omnetpp', 'perlbench', 'sjeng', 'soplex', 'sphinx3', 'xalancbmk']

#output_dir_base = "/scratch/cluster/akanksha/sniper_output_3level/{0}/{1}/{2}"
#output_dir_base = "/scratch/cluster/akanksha/sniper_output_replacement_1B/{0}/{1}/1"
#output_dir_base = "/u/ypei/Desktop/cs395t-lin/output/{0}/{1}/{2}"
output_dir_base = "/u/ypei/Desktop/cs395t-lin/output/{0}/{1}/1"

benchmark_output_dir_base = "{0}/{1}"

pinballs_dir_base = "/u/haowu/scratch/smets/sniper_pinpoints/cpu2006_pinballs/{0}/pinball_short.pp"
pinballs_weight_base = "/u/haowu/scratch/smets/sniper_simpoint/{0}/pinball.Data/t.weights"

def get_pinpoint_set(benchmark):
    pinballs_dir = pinballs_dir_base.format(benchmark)
    pinballs_weight_file = pinballs_weight_base.format(benchmark)
    pinpoint_set = {}
    pinpoint_weight = {}

    fp = open(pinballs_weight_file)
    count = 1
    line = fp.readline()

    while line:
        res = re.match("(\d+\.\d+) (\d+)", line)
        if res != None:
            #weight = line.split(' ', 1)[0]
            simpoint_no = int(res.group(2))
            weight = res.group(1)
    #        print simpoint_no, weight
            pinpoint_weight[simpoint_no+1] = float(weight)
            line = fp.readline()
            #count += 1

    fp.close()

    for file in os.listdir(pinballs_dir):
        res = re.match("(pinball_t0r(\d+)_(.+)).address", file)
        if res != None:
            simpoint_no = int(res.group(2))
            pinball_name = res.group(1)
            pinpoint_set[simpoint_no] = pinball_name

#            print simpoint_no, pinpoint_set[simpoint_no], pinpoint_weight[simpoint_no]

    return (pinpoint_set, pinpoint_weight)

def computeSpeedup(benchmark):
    #baseline_dir = output_dir_base.format('baseline_1B', benchmark)
    baseline_dir = output_dir_base.format('baseline', benchmark)
    #baseline_dir = output_dir_base.format('baseline_250M', benchmark)
    output_dir = output_dir_base.format(sys.argv[1], benchmark)

    try:
        baseline_res = sniper_lib.get_results(jobid = None, resultsdir = baseline_dir)
    except (KeyError, ValueError), e:
        print 'Failed to read stats:', e
        return 0

    try:
        output_res = sniper_lib.get_results(jobid = None, resultsdir = output_dir)
    except (KeyError, ValueError), e:
        print 'Failed to read stats:', e
        return 0

    baseline_results = baseline_res['results']
    output_results = output_res['results']

    baseline_ipc = baseline_results['ipc'][0]
    output_ipc = output_results['ipc'][0]
    speedup = 100*(output_ipc - baseline_ipc)/float(baseline_ipc)
    #print pinpoint, baseline_ipc, output_ipc, speedup
    return speedup


if __name__ == "__main__":

    if len(sys.argv) < 2:
        print "Usage: run_all.py output_dir"
        exit(-1)

    output_root_dir = output_dir_base.format(sys.argv[1], '', '')

    geomean = 1;
    for benchmark in benchmark_set:

        speedup = 1.0
        pinpoint_speedup = computeSpeedup(benchmark)
        speedup *= (100+pinpoint_speedup)
#            print pinpoint, weight, pinpoint_speedup

        geomean = geomean*(speedup ** (1.0/float(len(benchmark_set))))
        print benchmark, (speedup - 100)

    print "Geomean: ", geomean
