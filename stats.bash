
rm stats/summary.stats
benchmark=(
"astar 19"
"bzip2 19"
"gcc 20"
"libquantum 22"
"mcf 23"
"omnetpp 30"
"perlbench 12"
"sjeng 18"
"soplex 22"
"sphinx3 21"
"xalancbmk 23"
)
for i in "${benchmark[@]}" 
do
    echo $i
    ./a.out $i >> stats/summary.stats
done
