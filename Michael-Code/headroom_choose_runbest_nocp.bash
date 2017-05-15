#remove all dir in headroom and rebuild them
#benchmark=(

#"bzip2"
#"gcc"
#"libquantum"
#"mcf"
#"omnetpp"
#"perlbench"
#"sjeng"
#"soplex"
#"sphinx3"
#"xalancbmk"
#)
#already run astar
#gcc mcf soplex sphinx3
#bzip2 libquantum
benchmark=(
"astar"
"bzip2"
"gcc"
"omnetpp"
"perlbench"
"mcf"
"soplex"
"xalancbmk"
"libquantum"
"sphinx3"
)


for i in "${benchmark[@]}";
do
    cd "$i"
    echo "$i"
    for j in */; 
    do
        cd "$j" 
        echo "$j"
        #mkdir naive_hybrid
        #cp * naive_hybrid	
        ../../headroom_choosebest $(pwd)
        condor_submit run.condor
        cd ..
    done
    cd ..
#>> stats/summary.stats
done



