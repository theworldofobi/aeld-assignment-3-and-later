#!/bin/sh

if [ $# -ne 2 ]; then
  echo "ERROR (1) finder.sh: please run as finder.sh <filesdir> <searchstr>"
  exit 1
fi 

filesdir=$1
searchstr=$2

if [ ! -d $filesdir ]; then
  echo "ERROR (1) finder.sh: $filesdir is not a directory"
  exit 1
fi 

X=$(ls $filesdir -r1 | wc -l)

Y=$(grep -r -i $searchstr $filesdir | wc -l)

echo "The number of files are $X and the number of matching lines are $Y"

