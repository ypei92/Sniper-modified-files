#build run,condor and run.sh for each headroom experiment
benchmark=(
"astar"
"bzip2"
"gcc"
"libquantum"
"mcf"
"omnetpp"
"perlbench"
"sjeng"
"soplex"
"sphinx3"
"xalancbmk"
)
for i in "${benchmark[@]}";
do
    cd "$i"
    echo "$i"
    for j in */; 
    do
        cd "$j" 
        echo "$j"
	cd "headroom"
        for cfg in */;
        do
            cp $cfg.csv "pc_choice.csv"
            condor_submit ./run.condor
        done
        cd ../..
    done
    cd ..
#>> stats/summary.stats
done



