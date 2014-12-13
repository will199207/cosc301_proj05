#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

int data[2847] = { 0 }; 

void fix_orphans(uint8_t *image_buf, struct bpb33* bpb){
	int i = 0;
	uint16_t cluster = 0;
	while (i<=2847){
		if (data[i] == 0){ //unaccessed cluster
			cluster = get_fat_entry(cluster, image_buf, bpb);
			if(cluster != (CLUST_FREE & FAT12_MASK)){
				printf("Cluster #%d was freed becasue it was an orphan\n", i); 
				set_fat_entry(cluster, CLUST_FREE & FAT12_MASK, image_buf, bpb);
			}
		}
	i++; 
	}
}
				
				
			

int file_size_fat (struct bpb33* bpb, struct direntry *dirent, uint8_t *image_buf){
	printf("file size fat\n"); 
	//get the start cluster of the file
	uint16_t cluster = getushort(dirent->deStartCluster);
	//count the number of clusters the fat thinks a file has
    uint16_t cluster_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
	uint16_t cluster_old = 0;
	uint32_t size = 0;
	uint16_t cluster_next; 
	//run through the fat chain, getting the size that the fat thinks the file is
	while (!is_end_of_file(cluster)){
		data[cluster] = 1; 
		if (cluster == (CLUST_BAD & FAT12_MASK)){//if a bad cluster is found
			printf("Bad cluster found and removed\n");
			cluster_next = get_fat_entry(cluster, image_buf, bpb); //get the next cluster
			set_fat_entry(cluster_old, cluster_next, image_buf, bpb); //skip the current cluster
			set_fat_entry(cluster, CLUST_FREE & FAT12_MASK, image_buf, bpb); //free the current "bad" cluster
			cluster = cluster_next; //move to the correct next
			size--; //reduce the size by 1
		}
		size++; 
		//cluster_old = cluster; //remember old cluster if we hit a bad one
		cluster = get_fat_entry(cluster, image_buf, bpb); 
	}
	size = cluster_size*(size);
	return size;
}

void dirent_fix(uint32_t size, uint32_t fat_size, struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb){
	printf("direntfix\n"); 
	if(size>fat_size){ //need to fix the dirent
		printf("*****Size Error: File size description is larger than FAT indicates.\n"); 
		putulong( dirent->deFileSize, fat_size);
	

	}else{	//need to mark last cluster(s) as free
		printf("*****Size Error: File size description is smaller than FAT indicates.\n"); 
		uint16_t cluster = getushort(dirent->deStartCluster);
		uint16_t cluster_old = 0;
		uint16_t cluster_very_old = 0;
		while(fat_size > size){
			while(!is_end_of_file(cluster)){ //find last cluster marked as EOF
				cluster_very_old = cluster_old;
				cluster_old = cluster;
				cluster = get_fat_entry(cluster, image_buf, bpb);
			}
			set_fat_entry(cluster_old, FAT12_MASK&CLUST_EOFS, image_buf, bpb); //set the old EOF fat as empty 
			set_fat_entry(cluster_very_old, CLUST_EOFS&FAT12_MASK, image_buf, bpb); //set the new EOF
			fat_size = (fat_size - 512); //reduce the diff by the size of an eliminated cluster 
			cluster = getushort(dirent->deStartCluster); //reset
		}
	}
	
}


uint16_t print_dirent(struct direntry *dirent, int indent, struct bpb33* bpb, uint8_t *image_buf)
{
    uint16_t followclust = 0;
    int i;
    char name[9];
    char extension[4];
    uint32_t size;
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);
    if (name[0] == SLOT_EMPTY){
		return followclust;
    }

    /* skip over deleted entries */
	if (((uint8_t)name[0]) == SLOT_DELETED){
		return followclust;
	}

    if (((uint8_t)name[0]) == 0x2E){
	// dot entry ("." or "..")
	// skip it
        return followclust;
    }

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--) {
		if (name[i] == ' ') 
	  	  name[i] = '\0';
		else 
	    	break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--) {
		if (extension[i] == ' ') 
	    	extension[i] = '\0';
		else 
	   		break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN){
		// ignore any long file name extension entries
		//
		// printf("Win95 long-filename entry seq 0x%0x\n", dirent->deName[0]);
	}

    else if ((dirent->deAttributes & ATTR_VOLUME) != 0) {
		printf("Volume: %s\n", name);
    } 

    else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0){
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
		if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN){
		    file_cluster = getushort(dirent->deStartCluster);
		    followclust = file_cluster; 
		}
    }
	
    else{
		    /*
		     * a "regular" file entry
		     * print attributes, size, starting cluster, etc.
		     */
		int ro = (dirent->deAttributes & ATTR_READONLY) == ATTR_READONLY;
		int hidden = (dirent->deAttributes & ATTR_HIDDEN) == ATTR_HIDDEN;
		int sys = (dirent->deAttributes & ATTR_SYSTEM) == ATTR_SYSTEM;
		int arch = (dirent->deAttributes & ATTR_ARCHIVE) == ATTR_ARCHIVE;
		int fat_size = 0;
		fat_size = file_size_fat(bpb, dirent, image_buf);
		size = getulong(dirent->deFileSize);

		//check the results of the file_size_fat 
		int dif = fat_size-size;
		if(dif > 512 || dif<0){	
			dirent_fix(size, fat_size, dirent, image_buf, bpb); 	
		}
		printf("%s.%s (%u bytes) (starting cluster %d) %c%c%c%c\n", 
			   name, extension, size, getushort(dirent->deStartCluster),
			   ro?'r':' ', 
		           hidden?'h':' ', 
		           sys?'s':' ', 
		           arch?'a':' ');
		printf("	FAT size: %d\n", fat_size);
	}

    return followclust;
}
void follow_dir(uint16_t cluster, int indent,
		uint8_t *image_buf, struct bpb33* bpb)
{
    while (is_valid_cluster(cluster, bpb)) //check to make sure the cluster # is in the right region
    {
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb); //

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
	for ( ; i < numDirEntries; i++)
	{
            
            uint16_t followclust = print_dirent(dirent, indent, bpb, image_buf);
            if (followclust)
                follow_dir(followclust, indent+1, image_buf, bpb);
            dirent++;
	}

	cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}


void check_sizes(uint8_t *image_buf, struct bpb33 * bpb){
	//recursively find each file in the system
	//check that its fat chain size matches its dirent size
	//make changes if required
    uint16_t cluster = 0;
	struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb); //convert each cluster to an address, which is a dirent
	int i = 0;
    for ( ; i < bpb->bpbRootDirEnts; i++) //for the for the number of directories in the Root
    {
        uint16_t followclust = print_dirent(dirent, 0, bpb, image_buf); 
        if (is_valid_cluster(followclust, bpb)) //make sure its a valid cluster
            follow_dir(followclust, 1, image_buf, bpb); //recursive call to follow each dirent

        dirent++;
    }
}

int main(int argc, char** argv) {
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;

    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);
	//boot sector is OK?

	//run through the fat, compling a list of referenced clusters 

	check_sizes(image_buf, bpb);
	fix_orphans(image_buf, bpb); 
	
	//run the dirs/files, storing the metadata print out problems 

	//go back and fix the problems




    unmmap_file(image_buf, &fd);
    return 0;
}
