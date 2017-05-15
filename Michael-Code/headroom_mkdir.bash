#make the headroom dir
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
	rm -R "headroom"        
        mkdir "headroom"
        cd ..
    done
    cd ..
#>> stats/summary.stats
done



