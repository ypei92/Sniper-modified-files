#remove all dir in headroom and rebuild them
#benchmark=(
#"astar"
#"bzip2"
#"gcc"
#"libquantum"
#"mcf"
#"soplex"
#"sphinx3"
#"xalancbmk"
#"perlbench"
#)

#already run: 
#astar mcf soplex
#xalancbmk
#gcc
#bzip2
#sphinx3
#libquantum
#perlbench

benchmark=(
"libquantum"
"perlbench"
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
        
        for choice in */;
        do
            cd $choice
            chmod a+x run.sh
            #[ ! -f sim.out ] && condor_submit run.condor
            condor_submit run.condor
            cd ..
        done
        cd ../..
    done
    cd ..
#>> stats/summary.stats
done



