#!/bin/bash
# Provided by forum member blazerte.
# http://forums.slimdevices.com/showpost.php?p=443013&postcount=23
str=$(./squeezeslave -L | grep pulse)
./squeezeslave -r15 -o ${str%%: pulse}
