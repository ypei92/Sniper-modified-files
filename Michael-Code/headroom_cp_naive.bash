#remove all dir in headroom and rebuild them
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
        #echo "$j"
        mkdir naive_hybrid_2
        cp * naive_hybrid_2
        #../../headroom_choosebest $(pwd)
        cd ..
    done
    cd ..
#>> stats/summary.stats
done



