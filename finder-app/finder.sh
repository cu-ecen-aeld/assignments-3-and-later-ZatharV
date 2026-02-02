#!/bin/sh

#AI-chat: https:copilot.microsoft.com/shares/Wg6SSa5FNT5BSmBJ5DJvw
#Line 5 added based on chat
#sudo apt install tree

if [ $# -ne 2 ]
then
	echo "You should pass 2 arguments."
	echo "The order of arguments should be 'filesdir' and 'searchstr'."
	exit 1
fi

filesdir=$1
searchstr=$2

if [ ! -d "$filesdir" ]
then
	echo "The directory does not exist."
	exit 1
fi

#reference: https://chatgpt.com/share/698029c2-f794-8001-a3ed-bf2d59118082
X=$(find "$filesdir" -type f | wc -l)
Y=$(grep -r "$searchstr" "$filesdir" | wc -l)

echo "The number of files are $X and the number of matching lines are $Y"

exit 0