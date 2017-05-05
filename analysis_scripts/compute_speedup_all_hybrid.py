#!/usr/bin/python

import os, subprocess, re, sys, stat, sniper_lib


#benchmark_set = ['astar', 'bwaves', 'bzip2', 'cactusADM', 'calculix', 'gamess', 'gcc', 'GemsFDTD', 'gobmk', 'gromacs', 'h264ref', 'hmmer', 'lbm', 'leslie3d', 'libquantum', 'mcf', 'milc', 'omnetpp', 'perlbench', 'sjeng', 'soplex', 'sphinx3', 'tonto', 'wrf', 'xalancbmk', 'zeusmp']
#benchmark_set = ['astar', 'bwaves', 'bzip2', 'cactusADM', 'calculix', 'gcc', 'GemsFDTD', 'gobmk', 'gromacs', 'h264ref', 'hmmer', 'lbm', 'leslie3d', 'libquantum', 'mcf', 'milc', 'perlbench', 'soplex', 'sphinx3', 'tonto', 'wrf', 'xalancbmk', 'zeusmp', 'omnetpp']
#Irregular
benchmark_set = ['astar', 'bzip2', 'gcc', 'mcf', 'omnetpp', 'perlbench', 'soplex', 'sphinx3', 'xalancbmk', 'gobmk', 'hmmer', 'h264ref', 'libquantum']
#Regular
benchmark_set = ['bwaves', 'cactusADM', 'calculix', 'GemsFDTD', 'gromacs', 'lbm', 'leslie3d', 'milc', 'tonto', 'wrf', 'zeusmp']
#benchmark_set = ['mcf', 'bzip2', 'gcc', 'omnetpp', 'astar', 'soplex', 'sphinx3', 'xalancbmk', 'libquantum', 'zeusmp', 'GemsFDTD', 'lbm', 'milc', 'wrf']
#benchmark_set = ['bwaves', 'cactusADM', 'calculix', 'gobmk', 'gromacs', 'h264ref', 'hmmer', 'leslie3d', 'milc', 'perlbench', 'tonto', 'wrf']
benchmark_set = ['astar', 'gcc', 'libquantum', 'mcf', 'soplex', 'sphinx3', 'xalancbmk', 'omnetpp']
#benchmark_set = ['soplex']

# cs395t-lin
benchmark_set = ['astar', 'bzip2', 'gcc', 'libquantum', 'mcf', 'omnetpp', 'perlbench', 'sjeng', 'soplex', 'sphinx3', 'xalancbmk']

#output_dir_base = "/scratch/cluster/akanksha/sniper_output_3level/{0}/{1}/{2}"
#output_dir_base = "/scratch/cluster/akanksha/sniper_output/{0}/{1}/{2}"
output_dir_base = "/u/ypei/Desktop/cs395t-lin/output/{0}/{1}/{2}"

benchmark_output_dir_base = "{0}/{1}"

pinpoint_dir = "/u/haowu/scratch/smets/sniper_simpoint"
pinballs_dir_base = "/u/haowu/scratch/smets/sniper_simpoint/{0}/pinball.pp"
pinballs_file_base = "/u/haowu/scratch/smets/sniper_simpoint/{0}/pinball.pp/{1}"
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

def computeSpeedup(benchmark, pinpoint, pinball_name):
    baseline_dir = output_dir_base.format('baseline', benchmark, pinpoint)
    #baseline_dir = output_dir_base.format('baseline_noninclusive', benchmark, pinpoint)
    #baseline_dir = output_dir_base.format('lru_stride', benchmark, pinpoint)
    #baseline_dir = output_dir_base.format('baseline_nostlb', benchmark, pinpoint)
    output_dir = output_dir_base.format(sys.argv[1], benchmark, pinpoint)

    try:
        baseline_res = sniper_lib.get_results(jobid = None, resultsdir = baseline_dir)
    except (KeyError, ValueError), e:
        print 'Failed to read stats:', e, pinpoint
        return 0

    try:
        output_res = sniper_lib.get_results(jobid = None, resultsdir = output_dir)
    except (KeyError, ValueError), e:
        print 'Failed to read stats:', e, pinpoint
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
    if not os.path.exists(output_root_dir):
        os.mkdir(output_root_dir)

    geomean = 1;
    for benchmark in benchmark_set:
        pinpoint_set, pinpoint_weight = get_pinpoint_set(benchmark)
#        print pinpoint_set
#        print benchmark
        baseline_dir = output_dir_base.format('baseline', benchmark, '')
        if not os.path.exists(baseline_dir):
            print "Error opening ", baseline_dir 
            exit(-1)
        output_dir = output_dir_base.format(sys.argv[1], benchmark, '')
        if not os.path.exists(output_dir):
            print "Error opening ", output_dir 
            exit(-1)

        speedup = 1.0
        for pinpoint in pinpoint_set:
            weight = pinpoint_weight[pinpoint]
            pinpoint_speedup = computeSpeedup(benchmark, pinpoint, pinpoint_set[pinpoint])
            speedup *= (100+pinpoint_speedup) ** weight
#            print pinpoint, pinpoint_speedup

        geomean = geomean*(speedup ** (1.0/float(len(benchmark_set))))
        print benchmark, (speedup - 100)

    print "Geomean: ", geomean
