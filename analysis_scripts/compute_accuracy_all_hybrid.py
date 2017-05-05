#!/usr/bin/python

import os, subprocess, re, sys, stat, sniper_lib


#benchmark_set = ['astar', 'bwaves', 'bzip2', 'cactusADM', 'calculix', 'dealII', 'gamess', 'gcc', 'GemsFDTD', 'gobmk', 'gromacs', 'h264ref', 'hmmer', 'lbm', 'leslie3d', 'libquantum', 'mcf', 'milc', 'namd', 'omnetpp', 'perlbench', 'povray', 'sjeng', 'soplex', 'sphinx3', 'tonto', 'wrf', 'xalancbmk', 'zeusmp']
#benchmark_set = ['astar', 'bzip2', 'gcc', 'libquantum', 'mcf', 'perlbench', 'sjeng', 'soplex', 'sphinx3', 'xalancbmk', 'omnetpp']
#benchmark_set = ['astar', 'gcc', 'libquantum', 'mcf', 'soplex', 'sphinx3', 'xalancbmk', 'omnetpp']
### origin one in this file: benchmark_set = ['astar', 'gcc', 'mcf', 'soplex', 'sphinx3', 'xalancbmk', 'omnetpp']
#benchmark_set = ['bwaves', 'cactusADM', 'calculix', 'GemsFDTD', 'gromacs', 'lbm', 'leslie3d', 'milc', 'tonto', 'wrf', 'zeusmp']

#benchmark_set = ['astar', 'bzip2', 'cactusADM', 'calculix', 'gcc', 'GemsFDTD', 'gobmk', 'gromacs', 'h264ref', 'hmmer', 'lbm', 'leslie3d', 'libquantum', 'mcf', 'soplex', 'sphinx3', 'tonto', 'xalancbmk', 'zeusmp', 'omnetpp']

# according to run_all.py
# benchmark_set = ['astar', 'bzip2', 'gcc', 'libquantum', 'mcf', 'omnetpp', 'perlbench', 'sjeng', 'soplex', 'sphinx3', 'xalancbmk']
benchmark_set = ['astar', 'bzip2', 'gcc', 'libquantum', 'mcf', 'omnetpp', 'perlbench', 'soplex', 'sphinx3', 'xalancbmk']

pinpoint_dir = "/u/haowu/scratch/smets/sniper_simpoint"
#output_dir_base = "/scratch/cluster/akanksha/sniper_output_3level/{0}/{1}/{2}"
#output_dir_base = "/scratch/cluster/akanksha/sniper_output_bw/{0}/{1}/{2}"
#output_dir_base = "/scratch/cluster/akanksha/sniper_output_hybrid/{0}/{1}/{2}"
#output_dir_base = "/scratch/cluster/akanksha/sniper_output_prefetcher/{0}/{1}/{2}"
output_dir_base = "/u/ypei/Desktop/cs395t-lin/output/{0}/{1}/{2}"
#output_dir_base = "a/sniper_output_prefetcher/{0}/{1}/{2}"

#output_dir_base = "/projects/speedway/ajain/{0}/{1}/{2}"
#output_dir_base = "/projects/speedway/ajain/micro16/{0}/{1}/{2}"
#output_dir_base = "/scratch/cluster/akanksha/sniper_output/{0}/{1}/{2}"

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

        #    print simpoint_no, pinpoint_set[simpoint_no], pinpoint_weight[simpoint_no]

    return (pinpoint_set, pinpoint_weight)

def computeAccuracy(benchmark, pinpoint, pinball_name):
    output_dir = output_dir_base.format(sys.argv[1], benchmark, pinpoint)

    try:
        output_res = sniper_lib.get_results(jobid = None, resultsdir = output_dir)
    except (KeyError, ValueError), e:
        print 'Failed to read stats:', e
        return 0

    output_results = output_res['results']

    prefetch_hits = output_results['L2.hits-prefetch'][0]
    prefetch_init = output_results['L2.loads-prefetch'][0]
    if prefetch_init != 0:
        accuracy = 100*float(prefetch_hits)/float(prefetch_init)
    else:
        accuracy = 0
    #print pinpoint, accuracy
    return accuracy


if __name__ == "__main__":

    if len(sys.argv) < 3:
        print "Usage: run_all.py output_dir config_file"
        exit(-1)

    output_root_dir = output_dir_base.format(sys.argv[1], '', '')

    average_accuracy = 0
    count = 0

    for benchmark in benchmark_set:
        pinpoint_set, pinpoint_weight = get_pinpoint_set(benchmark)
#        print pinpoint_set
        #print benchmark
        output_dir = output_dir_base.format(sys.argv[1], benchmark, '')
        if not os.path.exists(output_dir):
            print "Error" 
            exit(-1)

        count += 1
        accuracy = 0
        for pinpoint in pinpoint_set:
            weight = pinpoint_weight[pinpoint]
            pinpoint_accuracy = computeAccuracy(benchmark, pinpoint, pinpoint_set[pinpoint])
            accuracy += weight * pinpoint_accuracy
#            print pinpoint, weight, pinpoint_accuracy

        average_accuracy += accuracy
        print benchmark, accuracy

    print "Average ", average_accuracy/count
