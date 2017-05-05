#!/usr/bin/python

import os, subprocess, re, sys, stat, sniper_lib


#benchmark_set = ['astar', 'bwaves', 'bzip2', 'cactusADM', 'calculix', 'dealII', 'gamess', 'gcc', 'GemsFDTD', 'gobmk', 'gromacs', 'h264ref', 'hmmer', 'lbm', 'leslie3d', 'libquantum', 'mcf', 'milc', 'namd', 'perlbench', 'povray', 'sjeng', 'soplex', 'sphinx3', 'tonto', 'wrf', 'xalancbmk', 'zeusmp', 'omnetpp']
#benchmark_set = ['astar', 'bzip2', 'gcc', 'libquantum', 'mcf', 'omnetpp', 'perlbench', 'sjeng', 'soplex', 'sphinx3', 'xalancbmk']
#benchmark_set = ['astar', 'gcc', 'libquantum', 'mcf', 'omnetpp', 'soplex', 'sphinx3', 'xalancbmk']
#benchmark_set = ['bwaves', 'cactusADM', 'calculix', 'GemsFDTD', 'gromacs', 'lbm', 'leslie3d', 'milc', 'tonto', 'wrf', 'zeusmp']
#benchmark_set = ['bwaves', 'cactusADM', 'calculix', 'GemsFDTD', 'gromacs', 'lbm', 'leslie3d', 'milc', 'tonto', 'wrf', 'zeusmp', 'astar', 'gcc', 'libquantum', 'mcf', 'soplex', 'sphinx3', 'xalancbmk', 'omnetpp']
#benchmark_set = ['milc']
#benchmark_set = ['astar', 'bzip2', 'cactusADM', 'calculix', 'gcc', 'GemsFDTD', 'gobmk', 'gromacs', 'h264ref', 'hmmer', 'lbm', 'leslie3d', 'libquantum', 'mcf', 'soplex', 'sphinx3', 'tonto', 'xalancbmk', 'zeusmp', 'omnetpp']

#Regular 
#file original one
#benchmark_set = ['cactusADM', 'calculix', 'GemsFDTD', 'h264ref', 'hmmer', 'lbm', 'leslie3d', 'libquantum', 'tonto', 'zeusmp', 'milc', 'sphinx3', 'gromacs']

#irregular
#benchmark_set = ['bzip2', 'gcc', 'mcf', 'omnetpp', 'perlbench', 'soplex', 'sphinx3', 'xalancbmk', 'gobmk']

# cs395t-lin
# benchmark_set = ['astar', 'bzip2', 'gcc', 'libquantum', 'mcf', 'omnetpp', 'perlbench', 'sjeng', 'soplex', 'sphinx3', 'xalancbmk']
benchmark_set = ['astar', 'bzip2', 'gcc', 'libquantum', 'mcf', 'omnetpp', 'perlbench', 'soplex', 'sphinx3', 'xalancbmk']

pinpoint_dir = "/u/haowu/scratch/smets/sniper_simpoint"
#output_dir_base = "/scratch/cluster/akanksha/sniper_output_hybrid/{0}/{1}/{2}"
#output_dir_base = "/scratch/cluster/akanksha/sniper_output_prefetcher/{0}/{1}/{2}"
output_dir_base = "/u/ypei/Desktop/cs395t-lin/output/{0}/{1}/{2}"
#output_dir_base = "/scratch/cluster/akanksha/sniper_output_bw/{0}/{1}/{2}"
#output_dir_base = "/projects/speedway/ajain/{0}/{1}/{2}"
#output_dir_base = "/projects/speedway/ajain/nol1mshr/{0}/{1}/{2}"
#output_dir_base = "/projects/speedway/ajain/micro16/{0}/{1}/{2}"
#output_dir_base = "/scratch/cluster/akanksha/sniper_output_3level/{0}/{1}/{2}"
#output_dir_base = "/scratch/cluster/akanksha/sniper_output/{0}/{1}/{2}"
#output_dir_base2 = "/scratch/cluster/haowu/smets/sniper_output/{0}/{1}/{2}"

benchmark_output_dir_base = "{0}/{1}"

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

            #print simpoint_no, pinpoint_set[simpoint_no], pinpoint_weight[simpoint_no]

    return (pinpoint_set, pinpoint_weight)

def computeCoverage(benchmark, pinpoint, pinball_name):
    baseline_dir = output_dir_base.format('baseline', benchmark, pinpoint)
    #baseline_dir = output_dir_base.format('baseline_noninclusive', benchmark, pinpoint)
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

    #baseline_misses = baseline_results['L3.load-misses'][0] + baseline_results['L3.store-misses'][0]
    #output_misses = output_results['L3.load-misses'][0] + output_results['L3.store-misses'][0]
    baseline_misses = baseline_results['L2.load-misses'][0]
    output_misses = output_results['L2.load-misses'][0]
    coverage = 100*(baseline_misses - output_misses)/float(baseline_misses)
    #print pinpoint, baseline_misses, output_misses, coverage
    return coverage


if __name__ == "__main__":

    if len(sys.argv) < 3:
        print "Usage: run_all.py output_dir config_file"
        exit(-1)

    output_root_dir = output_dir_base.format(sys.argv[1], '', '')

    average_coverage = 0
    count = 0
    for benchmark in benchmark_set:
        pinpoint_set, pinpoint_weight = get_pinpoint_set(benchmark)
#        print pinpoint_set
#        print benchmark
        baseline_dir = output_dir_base.format('baseline', benchmark, '')
        if not os.path.exists(baseline_dir):
            print "Error" 
            exit(-1)
        output_dir = output_dir_base.format(sys.argv[1], benchmark, '')
        if not os.path.exists(output_dir):
            print "Error" 
            exit(-1)
        
        count += 1
        coverage = 0
        for pinpoint in pinpoint_set:
            weight = pinpoint_weight[pinpoint]
            pinpoint_coverage = computeCoverage(benchmark, pinpoint, pinpoint_set[pinpoint])
            coverage += weight * pinpoint_coverage
    #        print pinpoint, weight, pinpoint_coverage
    #        print pinpoint, pinpoint_coverage

        average_coverage += coverage
        print benchmark, coverage
    
    print "Average ", average_coverage/count
