#!/bin/bash

# get the DomID of guest
sudo xl list
read -p "Please enter guest DomID:" n1
DOMID=$n1

###################
# different setting
###################

xl vcpu-pin $DOMID 0 0 0
xl vcpu-pin $DOMID 1 1 1
xl vcpu-pin $DOMID 2 2 2
xl vcpu-pin $DOMID 3 3 3
xl vcpu-pin $DOMID 4 4 4
xl vcpu-pin $DOMID 5 5 5
xl vcpu-pin $DOMID 6 6 6

xl vcpu-pin $DOMID 7 16 16
xl vcpu-pin $DOMID 8 17 17
xl vcpu-pin $DOMID 9 18 18
xl vcpu-pin $DOMID 10 19 19
xl vcpu-pin $DOMID 11 20 20
xl vcpu-pin $DOMID 12 21 21
xl vcpu-pin $DOMID 13 22 22

xl vcpu-pin $DOMID 14 32 32
xl vcpu-pin $DOMID 15 33 33
xl vcpu-pin $DOMID 16 34 34
xl vcpu-pin $DOMID 17 35 35
xl vcpu-pin $DOMID 18 36 36
xl vcpu-pin $DOMID 19 37 37
xl vcpu-pin $DOMID 20 38 38

xl vcpu-pin $DOMID 21 48 48
xl vcpu-pin $DOMID 22 49 49
xl vcpu-pin $DOMID 23 50 50
xl vcpu-pin $DOMID 24 51 51
xl vcpu-pin $DOMID 25 52 52
xl vcpu-pin $DOMID 26 53 53
xl vcpu-pin $DOMID 27 54 54

xl vcpu-pin $DOMID 28 8  8
xl vcpu-pin $DOMID 29 9 9
xl vcpu-pin $DOMID 30 10 10
xl vcpu-pin $DOMID 31 11 11
xl vcpu-pin $DOMID 32 12 12
xl vcpu-pin $DOMID 33 13 13
xl vcpu-pin $DOMID 34 14 14



xl vcpu-pin $DOMID 35 24 24
xl vcpu-pin $DOMID 36 25 25
xl vcpu-pin $DOMID 37 26 26
xl vcpu-pin $DOMID 38 27 27
xl vcpu-pin $DOMID 39 28 28
xl vcpu-pin $DOMID 40 29 29
xl vcpu-pin $DOMID 41 30 30

xl vcpu-pin $DOMID 42 40 40
xl vcpu-pin $DOMID 43 41 41
xl vcpu-pin $DOMID 44 42 42
xl vcpu-pin $DOMID 45 43 43
xl vcpu-pin $DOMID 46 44 44
xl vcpu-pin $DOMID 47 45 45
xl vcpu-pin $DOMID 48 46 46

xl vcpu-pin $DOMID 49 56 56
xl vcpu-pin $DOMID 50 57 57
xl vcpu-pin $DOMID 51 58 58
xl vcpu-pin $DOMID 52 59 59
xl vcpu-pin $DOMID 53 60 60
xl vcpu-pin $DOMID 54 61 61
xl vcpu-pin $DOMID 55 62 62

#####################
# different setting #
#####################

# xl vcpu-pin $DOMID 0 0 0
# xl vcpu-pin $DOMID 1 1 1
# xl vcpu-pin $DOMID 2 2 2
# xl vcpu-pin $DOMID 3 3 3
# xl vcpu-pin $DOMID 4 4 4
# xl vcpu-pin $DOMID 5 5 5
# xl vcpu-pin $DOMID 6 6 6
# 
# xl vcpu-pin $DOMID 7   8   8    
# xl vcpu-pin $DOMID 8   9   9    
# xl vcpu-pin $DOMID 9   10 10    
# xl vcpu-pin $DOMID 10  11 11    
# xl vcpu-pin $DOMID 11  12 12    
# xl vcpu-pin $DOMID 12  13 13    
# xl vcpu-pin $DOMID 13  14 14    
# 
# xl vcpu-pin $DOMID 14  16 16
# xl vcpu-pin $DOMID 15  17 17
# xl vcpu-pin $DOMID 16  18 18
# xl vcpu-pin $DOMID 17  19 19
# xl vcpu-pin $DOMID 18  20 20
# xl vcpu-pin $DOMID 19  21 21
# xl vcpu-pin $DOMID 20  22 22
# 
# xl vcpu-pin $DOMID 21  24 24
# xl vcpu-pin $DOMID 22  25 25
# xl vcpu-pin $DOMID 23  26 26
# xl vcpu-pin $DOMID 24  27 27
# xl vcpu-pin $DOMID 25  28 28
# xl vcpu-pin $DOMID 26  29 29
# xl vcpu-pin $DOMID 27  30 30
# 
# xl vcpu-pin $DOMID 28 32 32     
# xl vcpu-pin $DOMID 29 33 33     
# xl vcpu-pin $DOMID 30 34 34     
# xl vcpu-pin $DOMID 31 35 35     
# xl vcpu-pin $DOMID 32 36 36     
# xl vcpu-pin $DOMID 33 37 37     
# xl vcpu-pin $DOMID 34 38 38     
# 
# 
# 
# xl vcpu-pin $DOMID 35 40 40    
# xl vcpu-pin $DOMID 36 41 41    
# xl vcpu-pin $DOMID 37 42 42    
# xl vcpu-pin $DOMID 38 43 43    
# xl vcpu-pin $DOMID 39 44 44    
# xl vcpu-pin $DOMID 40 45 45    
# xl vcpu-pin $DOMID 41 46 46    
# 
# xl vcpu-pin $DOMID 42 48 48 
# xl vcpu-pin $DOMID 43 49 49
# xl vcpu-pin $DOMID 44 50 50
# xl vcpu-pin $DOMID 45 51 51
# xl vcpu-pin $DOMID 46 52 52
# xl vcpu-pin $DOMID 47 53 53
# xl vcpu-pin $DOMID 48 54 54
# 
# xl vcpu-pin $DOMID 49 56 56
# xl vcpu-pin $DOMID 50 57 57
# xl vcpu-pin $DOMID 51 58 58
# xl vcpu-pin $DOMID 52 59 59
# xl vcpu-pin $DOMID 53 60 60
# xl vcpu-pin $DOMID 54 61 61
# xl vcpu-pin $DOMID 55 62 62

xl vcpu-list $DOMID
# xl info -n

echo "=============pin dom0============="
#xl vcpu-pin 0 0    7,39   7,39
#xl vcpu-pin 0 1    7,39   7,39
#xl vcpu-pin 0 2    7,39   7,39
#xl vcpu-pin 0 3    7,39   7,39
#xl vcpu-pin 0 4    7,39   7,39
#xl vcpu-pin 0 5    7,39   7,39
#xl vcpu-pin 0 6    7,39   7,39
#xl vcpu-pin 0 7    7,39   7,39
for cpu in {0..63}; do
	xl vcpu-pin 0 $cpu    $cpu   $cpu
done

#for cpu in {0..7}; do
#	xl vcpu-pin 0 $cpu    7   7
#done
#for cpu in {32..39}; do
#	xl vcpu-pin 0 $cpu    39   39
#done
#
#
#for cpu in {8..15}; do
#	xl vcpu-pin 0 $cpu    15   15
#done
#for cpu in {40..47}; do
#	xl vcpu-pin 0 $cpu    47   47
#done
#
#for cpu in {16..23}; do
#	xl vcpu-pin 0 $cpu    23   23
#done
#for cpu in {48..55}; do
#	xl vcpu-pin 0 $cpu    55   55
#done
#
#
#for cpu in {24..31}; do
#	xl vcpu-pin 0 $cpu    31   31
#done
#for cpu in {56..63}; do
#	xl vcpu-pin 0 $cpu    63   63
#done



#xl vcpu-pin 0 0-7,32-39    7,39   7,39
#xl vcpu-pin 0 8-15,40-47   15,47  15,47
#xl vcpu-pin 0 16-23,48-55  23,55  23,55
#xl vcpu-pin 0 24-31,56-63  31,63  31,63

xl vcpu-list 0
