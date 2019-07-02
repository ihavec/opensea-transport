//
// Do NOT modify or remove this copyright and license
//
// Copyright (c) 2012 - 2018 Seagate Technology LLC and/or its Affiliates, All Rights Reserved
//
// This software is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
//
// ******************************************************************************************
// 
#include <stdio.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h> // for close
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h> //for mmap pci reads. Potential to move. 
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <libgen.h>//for basename and dirname
#include "vm_helper.h"
#include "cmds.h"
#include "scsi_helper_func.h"
#include "ata_helper_func.h"
#if !defined(DISABLE_NVME_PASSTHROUGH)
#include "nvme_helper_func.h"
#endif

#if defined(DEGUG_SCAN_TIME)
#include "common_platform.h"
#endif

extern bool validate_Device_Struct(versionBlock);


void decipher_maskedStatus( unsigned char maskedStatus )
{
    if (CHECK_CONDITION == maskedStatus)
        printf("CHECK CONDITION\n");
    else if (BUSY == maskedStatus)
        printf("BUSY\n");
    else if (COMMAND_TERMINATED == maskedStatus)
        printf("COMMAND TERMINATED\n");
    else if (QUEUE_FULL == maskedStatus)
        printf("QUEUE FULL\n");
}

// Local helper functions for debugging
void print_io_hdr( sg_io_hdr_t *pIo )
{
    time_t time_now;
    time_now = time(NULL);
    printf("\n%s: %s---------------------------------\n", __FUNCTION__, ctime(&time_now));
    printf("type int interface_id %d\n", pIo->interface_id);           /* [i] 'S' (required) */
    printf("type int  dxfer_direction %d\n", pIo->dxfer_direction);        /* [i] */
    printf("type unsigned char cmd_len 0x%x\n", pIo->cmd_len);      /* [i] */
    printf("type unsigned char mx_sb_len 0x%x\n", pIo->mx_sb_len);    /* [i] */
    printf("type unsigned short iovec_count 0x%x\n", pIo->iovec_count); /* [i] */
    printf("type unsigned int dxfer_len %d\n", pIo->dxfer_len);     /* [i] */
    printf("type void * dxferp %p\n", (unsigned int *)pIo->dxferp);              /* [i], [*io] */
    printf("type unsigned char * cmdp %p\n", (unsigned int *)pIo->cmdp);       /* [i], [*i]  */
    printf("type unsigned char * sbp %p\n", (unsigned int *)pIo->sbp);        /* [i], [*o]  */
    printf("type unsigned int timeout %d\n", pIo->timeout);       /* [i] unit: millisecs */
    printf("type unsigned int flags 0x%x\n", pIo->flags);         /* [i] */
    printf("type int pack_id %d\n", pIo->pack_id);                /* [i->o] */
    printf("type void * usr_ptr %p\n", (unsigned int *)pIo->usr_ptr);             /* [i->o] */
    printf("type unsigned char status 0x%x\n", pIo->status);       /* [o] */
    printf("type unsigned char maskedStatus 0x%x\n", pIo->masked_status); /* [o] */
    printf("type unsigned char msg_status 0x%x\n", pIo->msg_status);   /* [o] */
    printf("type unsigned char sb_len_wr 0x%x\n", pIo->sb_len_wr);    /* [o] */
    printf("type unsigned short host_status 0x%x\n", pIo->host_status); /* [o] */
    printf("type unsigned short driver_status 0x%x\n", pIo->driver_status); /* [o] */
    printf("type int resid %d\n", pIo->resid);                  /* [o] */
    printf("type unsigned int duration %d\n", pIo->duration);      /* [o] */
    printf("type unsigned int info 0x%x\n", pIo->info);          /* [o] */
    printf("-----------------------------------------\n");
}

static int sg_filter( const struct dirent *entry )
{
    return !strncmp("sg", entry->d_name, 2);
}

static int drive_filter( const struct dirent *entry )
{
    int driveHandle = strncmp("t10", entry->d_name, 3);

    if(driveHandle != 0) 
    {
        /**
         * Its not a SATA or NVMe. 
         * Lets check if it is SAS (starts with "naa.") 
         */

        driveHandle = strncmp("naa.", entry->d_name, 4);

        if(driveHandle != 0) 
        {
            return !driveHandle;
        }
    }

    driveHandle = strncmp("t10.NVMe", entry->d_name, 8);

    if(driveHandle == 0) 
    {
        return driveHandle;
    }

    char* partition = strpbrk(entry->d_name,":");
    if(partition != NULL)
    {
        return !driveHandle;
    }
    else
    {
        return driveHandle;
    }

}

//get sd devices, but ignore any partition number information since that isn't something we can actually send commands to
static int sd_filter( const struct dirent *entry )
{
    int sdHandle = strncmp("sd",entry->d_name,2);
    if(sdHandle != 0)
    {
      return !sdHandle;
    }
    char* partition = strpbrk(entry->d_name,"0123456789");
    if(partition != NULL)
    {
        return sdHandle;
    }
    else
    {
        return !sdHandle;
    }
}


//This function is not currently used or tested...if we need to make more changes for pre-2.6 kernels, we may need this.
//bool does_Kernel_Support_SysFS_Link_Mapping()
//{
//    bool linkMappingSupported = false;
//    //kernel version 2.6 and higher is required to map the handles between sg and sd/sr/st/scd
//    OSVersionNumber linuxVersion;
//    memset(&linuxVersion, 0, sizeof(OSVersionNumber));
//    if(SUCCESS == get_Operating_System_Version_And_Name(&linuxVersion, NULL))
//    {
//        if (linuxVersion.versionType.linuxVersion.kernelVersion >= 2 && linuxVersion.versionType.linuxVersion.majorVersion >= 6)
//        {
//            linkMappingSupported = true;
//        }
//    }
//    return linkMappingSupported;
//}

bool is_Block_Device_Handle(char *handle)
{
    bool isBlockDevice = false;
    if (handle && strlen(handle))
    {
        if(strstr(handle,"sd") || strstr(handle, "st") || strstr(handle, "sr") || strstr(handle, "scd"))
        {
            isBlockDevice = true;
        }
    }
    return isBlockDevice;
}

bool is_SCSI_Generic_Handle(char *handle)
{
    bool isGenericDevice = false;
    if (handle && strlen(handle))
    {
        if(strstr(handle,"sg") && !strstr(handle, "bsg"))
        {
            isGenericDevice = true;
        }
    }
    return isGenericDevice;
}

bool is_Block_SCSI_Generic_Handle(char *handle)
{
    bool isBlockGenericDevice = false;
    if (handle && strlen(handle))
    {
        if(strstr(handle,"bsg"))
        {
            isBlockGenericDevice = true;
        }
    }
    return isBlockGenericDevice;
}


//while similar to the function below, this is used only by get_Device to set up some fields in the device structure for the above layers
static void set_Device_Fields_From_Handle(const char* handle, tDevice *device)
{
    //check if it's a block handle, bsg, or scsi_generic handle, then setup the path we need to read.
    if (handle && device)
    {
        if (strstr(handle,"nvme") != NULL)
        {
            device->drive_info.interface_type = NVME_INTERFACE;
            device->drive_info.drive_type = NVME_DRIVE;
        }
        else //not NVMe, so we need to do some investigation of the handle. NOTE: this requires 2.6 and later kernel since it reads a link in the /sys/class/ filesystem
        {
            bool incomingBlock = false;//only set for SD!
            bool bsg = false;
            char incomingHandleClassPath[PATH_MAX] = { 0 };
            //char *incomingClassName = NULL;
            strcat(incomingHandleClassPath, "/sys/class/");
            if (is_Block_Device_Handle((char*)handle))
            {
                strcat(incomingHandleClassPath, "block/");
                incomingBlock = true;
                //incomingClassName = strdup("block");
            }
            else if (is_Block_SCSI_Generic_Handle((char*)handle))
            {
                bsg = true;
                strcat(incomingHandleClassPath, "bsg/");
                //incomingClassName = strdup("bsg");
            }
            else if (is_SCSI_Generic_Handle((char*)handle))
            {
                strcat(incomingHandleClassPath, "scsi_generic/");
                //incomingClassName = strdup("scsi_generic");
            }
            else
            {
                //unknown. Time to exit gracefully
                device->drive_info.interface_type = SCSI_INTERFACE;
                device->drive_info.drive_type = UNKNOWN_DRIVE;
                device->drive_info.media_type = MEDIA_UNKNOWN;
                return;
            }
            //first make sure this directory exists
            struct stat inHandleStat;
            if (stat(incomingHandleClassPath, &inHandleStat) == 0 && S_ISDIR(inHandleStat.st_mode))
            {
                struct stat link;
                strcat(incomingHandleClassPath, basename((char*)handle));
                //now read the link with the handle appended on the end
                if (lstat(incomingHandleClassPath,&link) == 0 && S_ISLNK(link.st_mode))
                {
                    char inHandleLink[PATH_MAX] = { 0 };
                    if (readlink(incomingHandleClassPath, inHandleLink, PATH_MAX) > 0)
                    {
                        //Read the link and set up all the fields we want to setup.
                        //Start with setting the device interface
                        //example ata device link: ../../devices/pci0000:00/0000:00:1f.2/ata8/host8/target8:0:0/8:0:0:0/scsi_generic/sg2
                        //example usb device link: ../../devices/pci0000:00/0000:00:1c.1/0000:03:00.0/usb4/4-1/4-1:1.0/host21/target21:0:0/21:0:0:0/scsi_generic/sg4
                        //example sas device link: ../../devices/pci0000:00/0000:00:1c.0/0000:02:00.0/host0/port-0:0/end_device-0:0/target0:0:0/0:0:0:0/scsi_generic/sg3
                        //example firewire device link: ../../devices/pci0000:00/0000:00:1c.5/0000:04:00.0/0000:05:09.0/0000:0b:00.0/0000:0c:02.0/fw1/fw1.0/host13/target13:0:0/13:0:0:0/scsi_generic/sg3
                        //example sata over sas device link: ../../devices/pci0000:00/0000:00:1c.0/0000:02:00.0/host0/port-0:1/end_device-0:1/target0:0:1/0:0:1:0/scsi_generic/sg5
                        if (strstr(inHandleLink,"ata") != 0)
                        {
                            #if defined (_DEBUG)
                            printf("ATA interface!\n");
                            #endif
                            device->drive_info.interface_type = IDE_INTERFACE;
                        }
                        else if (strstr(inHandleLink,"usb") != 0)
                        {
                            #if defined (_DEBUG)
                            printf("USB interface!\n");
                            #endif
                            device->drive_info.interface_type = USB_INTERFACE;
                        }
                        else if (strstr(inHandleLink,"fw") != 0)
                        {
                            #if defined (_DEBUG)
                            printf("FireWire interface!\n");
                            #endif
                            device->drive_info.interface_type = IEEE_1394_INTERFACE;
                        }
                        //if the link doesn't conatin ata or usb in it, then we are assuming it's scsi since scsi doesn't have a nice simple string to check
                        else
                        {
                            #if defined (_DEBUG)
                            printf("SCSI interface!\n");
                            #endif
                            device->drive_info.interface_type = SCSI_INTERFACE;
                        }
                        char *baseLink = basename(inHandleLink);
                        //Now we will set up the device name, etc fields in the os_info structure.
                        if (bsg)
                        {
                            sprintf(device->os_info.name, "/dev/bsg/%s", baseLink);
                        }
                        else
                        {
                            sprintf(device->os_info.name, "/dev/%s", baseLink);
                        }
                        sprintf(device->os_info.friendlyName, "%s", baseLink);

                        //printf("getting SCSI address\n");
                        //set the scsi address field
                        char *scsiAddress = basename(dirname(dirname(inHandleLink)));//SCSI address should be 2nd from the end of the link
                        if (scsiAddress)
                        {
                            char *token = strtok(scsiAddress, ":");
                            uint8_t counter = 0;
                            while (token)
                            {
                                switch (counter)
                                {
                                case 0://host
                                    device->os_info.scsiAddress.host = (uint8_t)atoi(token);
                                    break;
                                case 1://bus
                                    device->os_info.scsiAddress.channel = (uint8_t)atoi(token);
                                    break;
                                case 2://target
                                    device->os_info.scsiAddress.target = (uint8_t)atoi(token);
                                    break;
                                case 3://lun
                                    device->os_info.scsiAddress.lun = (uint8_t)atoi(token);
                                    break;
                                default:
                                    break;
                                }
                                token = strtok(NULL, ":");
                                ++counter;
                            }
                            if (counter >= 4)
                            {
                                device->os_info.scsiAddressValid = true;
                            }
                        }
                        //printf("attempting to map the handle\n");
                        //Lastly, call the mapping function to get the matching block handle and check what we got to set ATAPI, TAPE or leave as-is. Setting these is necessary to prevent talking to ATAPI as HDD due to overlapping A1h opcode
                        char *block = NULL;
                        char *gen = NULL;
                        if (SUCCESS == map_Block_To_Generic_Handle((char*)handle, &gen, &block))
                        {
                            //printf("successfully mapped the handle. gen = %s\tblock=%s\n", gen, block);
                            //Our incoming handle SHOULD always be sg/bsg, but just in case, we need to check before we setup the second handle (mapped handle) information
                            if (incomingBlock)
                            {
                                //block device handle was sent into here (and we made it this far...unlikely)
                                //Secondary handle will be a generic handle
                                if (is_Block_SCSI_Generic_Handle(gen))
                                {
                                    device->os_info.secondHandleValid = true;
                                    sprintf(device->os_info.secondName, "/dev/bsg/%s", gen);
                                    sprintf(device->os_info.secondFriendlyName, "%s", gen);
                                }
                                else
                                {
                                    device->os_info.secondHandleValid = true;
                                    sprintf(device->os_info.secondName, "/dev/%s", gen);
                                    sprintf(device->os_info.secondFriendlyName, "%s", gen);
                                }
                            }
                            else
                            {
                                //generic handle was sent in
                                //secondary handle will be a block handle
                                device->os_info.secondHandleValid = true;
                                sprintf(device->os_info.secondName, "/dev/%s", block);
                                sprintf(device->os_info.secondFriendlyName, "%s", block);
                            }

                            if (strstr(block, "sr") || strstr(block, "scd"))
                            {
                                device->drive_info.drive_type = ATAPI_DRIVE;
                            }
                            else if (strstr(block, "st"))
                            {
                                device->drive_info.drive_type = LEGACY_TAPE_DRIVE;
                            }
                        }
                        //printf("Finish handle mapping\n");
                        safe_Free(block);
                        safe_Free(gen);
                    }
                    else
                    {
                        //couldn't read the link...for who knows what reason...
                    }
                }
                else
                {
                    //Not a link...nothing further to do
                }
            }
        }
    }
    return;
}

//map a block handle (sd) to a generic handle (sg or bsg)
//incoming handle can be either sd, sg, or bsg type
//TODO: handle kernels before 2.6 in some other way. This depends on mapping in the file system provided by 2.6 and later.
int map_Block_To_Generic_Handle(char *handle, char **genericHandle, char **blockHandle)
{
    if (handle == NULL)
    {
        return BAD_PARAMETER;
    }
    //if the handle passed in contains "nvme" then we know it's a device on the nvme interface
    if (strstr(handle,"nvme") != NULL)
    {
        return NOT_SUPPORTED;
    }
    else
    {
        bool incomingBlock = false;//only set for SD!
        char incomingHandleClassPath[PATH_MAX] = { 0 };
        char *incomingClassName = NULL;
        strcat(incomingHandleClassPath, "/sys/class/");
        if (is_Block_Device_Handle(handle))
        {
            strcat(incomingHandleClassPath, "block/");
            incomingBlock = true;
            incomingClassName = strdup("block");
        }
        else if (is_Block_SCSI_Generic_Handle(handle))
        {
            strcat(incomingHandleClassPath, "bsg/");
            incomingClassName = strdup("bsg");
        }
        else if (is_SCSI_Generic_Handle(handle))
        {
            strcat(incomingHandleClassPath, "scsi_generic/");
            incomingClassName = strdup("scsi_generic");
        }
        //first make sure this directory exists
        struct stat inHandleStat;
        if (stat(incomingHandleClassPath, &inHandleStat) == 0 && S_ISDIR(inHandleStat.st_mode))
        {
            strcat(incomingHandleClassPath, basename(handle));
            //now read the link with the handle appended on the end
            char inHandleLink[PATH_MAX] = { 0 };
            if (readlink(incomingHandleClassPath, inHandleLink, PATH_MAX) > 0)
            {
                //printf("full in handleLink = %s\n", inHandleLink);
                //now we need to map it to a generic handle (sg...if sg not available, bsg)
                const char* scsiGenericClass = "/sys/class/scsi_generic/";
                const char* bsgClass = "/sys/class/bsg/";
                const char* blockClass = "/sys/class/block/";
                struct stat mapStat;
                char classPath[PATH_MAX] = { 0 };
                bool bsg = false;
                if (incomingBlock)
                {
                    //check for sg, then bsg
                    if (stat(scsiGenericClass, &mapStat) == 0 && S_ISDIR(mapStat.st_mode))
                    {
                        strcpy(classPath, scsiGenericClass);
                    }
                    else if (stat(bsgClass, &mapStat) == 0 && S_ISDIR(mapStat.st_mode))
                    {
                        strcpy(classPath, bsgClass);
                        bsg = true;
                    }
                    else
                    {
                        //printf ("could not map to generic class");
                        safe_Free(incomingClassName);
                        return NOT_SUPPORTED;
                    }
                }
                else
                {
                    //check for block
                    strcpy(classPath, blockClass);
                    if (!(stat(classPath, &mapStat) == 0 && S_ISDIR(mapStat.st_mode)))
                    {
                        //printf ("could not map to block class");
                        safe_Free(incomingClassName);
                        return NOT_SUPPORTED;
                    }
                }
                //now we need to loop through each think in the class folder, read the link, and check if we match.
                struct dirent **classList;
                int numberOfItems = scandir(classPath, &classList, NULL /*not filtering anything. Just go through each item*/, alphasort);
                for (int iter = 0; iter < numberOfItems; ++iter)
                {
                    //printf("item = %s\n", classList[iter]->d_name);
                    //now we need to read the link for classPath/d_name into a buffer...then compare it to the one we read earlier.
                    char temp[PATH_MAX] = { 0 };
                    strcpy(temp, classPath);
                    strcat(temp, classList[iter]->d_name);
                    struct stat tempStat;
                    if (lstat(temp,&tempStat) == 0 && S_ISLNK(tempStat.st_mode))/*check if this is a link*/
                    {
                        char mapLink[PATH_MAX] = { 0 };
                        if (readlink(temp, mapLink, PATH_MAX) > 0)
                        {
                            char *className = NULL;
                            //printf("read link as: %s\n", mapLink);
                            //now, we need to check the links and see if they match.
                            //NOTE: If we are in the block class, we will see sda, sda1, sda 2. These are all matches (technically)
                            //      We SHOULD match on the first disk without partition numbers since we did alphasort
                            //We need to match up until the class name (ex: block, bsg, scsi_generic)
                            if (incomingBlock)//block class
                            {
                                className = (char*)calloc(strlen("scsi_generic") + 1, sizeof(char));
                                if (className)
                                {
                                    strcat(className, "scsi_generic");
                                }
                            }
                            else if (bsg) //bsg class
                            {
                                className = (char*)calloc(strlen("bsg") + 1, sizeof(char));
                                if (className)
                                {
                                    strcat(className, "bsg");
                                }
                            }
                            else //scsi_generic class
                            {
                                className = (char*)calloc(strlen("block") + 1, sizeof(char));
                                if (className)
                                {
                                    strcat(className, "block");
                                }
                            }
                            if (className)
                            {
                                char *classPtr = strstr(mapLink, className);
                                //need to match up to the classname
                                if (NULL != classPtr && strncmp(mapLink, inHandleLink, (classPtr - mapLink)) == 0)
                                {
                                    if (incomingBlock)
                                    {
                                        *blockHandle = strndup(basename(handle), strlen(basename(handle)));
                                        *genericHandle = strdup(basename(classPtr));
                                    }
                                    else
                                    {
                                        *blockHandle = strndup(basename(classPtr), strlen(basename(classPtr)));
                                        *genericHandle = strdup(basename(handle));
                                    }
                                    safe_Free(className);
                                    safe_Free(incomingClassName);
                                    return SUCCESS;
                                    break;//found a match, exit the loop
                                }
                            }
                            safe_Free(className);
                        }
                    }
                }
                safe_Free(classList);
            }
            else
            {
                //not a link, or some other error....probably an old kernel
                safe_Free(incomingClassName);
                return NOT_SUPPORTED;
            }
        }
        else
        {
            //Mapping is not supported...probably an old kernel
            safe_Free(incomingClassName);
            return NOT_SUPPORTED;
        }
        safe_Free(incomingClassName);
    }
    return UNKNOWN;
}
//#endif

#define LIN_MAX_HANDLE_LENGTH 16
int get_Device(const char *filename, tDevice *device)
{
    char *deviceHandle = NULL;
    int ret = SUCCESS, k = 0;
    int rc;
    struct nvme_adapter_list nvmeAdptList;
    bool isScsi = false;
    char *nvmeDevName;

    /**
     * In VMWare NVMe device the drivename (for NDDK) 
     * always starts with "vmhba" (e.g. vmhba1) 
     */

    nvmeDevName = strstr(filename, "vmhba");
    isScsi = (nvmeDevName == NULL) ? true : false;

    //printf("Getting device for %s\n", filename);

    /**
     * List down both NVMe and HDD/SSD drives 
     * Get the device after matching the name 
     */
    deviceHandle = strdup(filename);

    if(isScsi) 
    {
        #if defined (_DEBUG)
        printf("This is a SCSI drive\n");
        printf("Attempting to open %s\n", deviceHandle);
        #endif
        // Note: We are opening a READ/Write flag
        if ((device->os_info.fd = open(deviceHandle, O_RDWR | O_NONBLOCK)) < 0)
        {
            perror("open");
            device->os_info.fd = errno;
            printf("open failure\n");
            printf("Error: ");
            print_Errno_To_Screen(errno);
            if (device->os_info.fd == EACCES) 
            {
                safe_Free(deviceHandle);
                return PERMISSION_DENIED;
            }
            else
            {
                safe_Free(deviceHandle);
                return FAILURE;
            }
        }
    
        //Adding support for different device discovery options. 
        if (device->dFlags == OPEN_HANDLE_ONLY)
        {
            //set scsi interface and scsi drive until we know otherwise
            device->drive_info.drive_type = SCSI_DRIVE;
            device->drive_info.interface_type = SCSI_INTERFACE;
            device->drive_info.media_type = MEDIA_HDD;
            set_Device_Fields_From_Handle(deviceHandle, device);
            safe_Free(deviceHandle);
            return ret;
        }
        //\\TODO: Add support for other flags. 
    
        if ((device->os_info.fd >= 0) && (ret == SUCCESS))
        {
            #if defined (_DEBUG)
            printf("Getting SG driver version\n");
            #endif

            /**
             * SG_GET_VERSION_NUM is currently not supported for VMWare 
             * SG_IO. 
             */
            #if 0
            // Check we have a valid device by trying an ioctl
            // From http://tldp.org/HOWTO/SCSI-Generic-HOWTO/pexample.html
            if ((ioctl(device->os_info.fd, SG_GET_VERSION_NUM, &k) < 0) || (k < 30000))
            {
                printf("%s: SG_GET_VERSION_NUM on %s failed version=%d\n", __FUNCTION__, filename,k);
                perror("SG_GET_VERSION_NUM");
                close(device->os_info.fd);
            }

            //http://www.faqs.org/docs/Linux-HOWTO/SCSI-Generic-HOWTO.html#IDDRIVER
            device->os_info.sgDriverVersion.driverVersionValid = true;
            device->os_info.sgDriverVersion.majorVersion = (uint8_t)(k / 10000);
            device->os_info.sgDriverVersion.minorVersion = (uint8_t)((k - (device->os_info.sgDriverVersion.majorVersion * 10000)) / 100);
            device->os_info.sgDriverVersion.revision = (uint8_t)(k - (device->os_info.sgDriverVersion.majorVersion * 10000) - (device->os_info.sgDriverVersion.minorVersion * 100));
            #endif

            //set the OS Type
            device->os_info.osType = OS_ESX;

            memcpy(device->os_info.name, deviceHandle, strlen(deviceHandle) + 1);

            //set scsi interface and scsi drive until we know otherwise
            device->drive_info.drive_type = SCSI_DRIVE;
            device->drive_info.interface_type = SCSI_INTERFACE;
            device->drive_info.media_type = MEDIA_HDD;
            //now have the device information fields set
            #if defined (_DEBUG)
            printf("Setting interface, drive type, secondary handles\n");
            #endif

            //set_Device_Fields_From_Handle(deviceHandle, device);
            device->drive_info.interface_type = SCSI_INTERFACE;
            device->drive_info.drive_type = UNKNOWN_DRIVE;
            device->drive_info.media_type = MEDIA_UNKNOWN;

            #if defined (_DEBUG)
            printf("name = %s\t friendly name = %s\n2ndName = %s\t2ndFName = %s\n",
                   device->os_info.name,
                   device->os_info.friendlyName,
                   device->os_info.secondName,
                   device->os_info.secondFriendlyName
                   );
            printf("h:c:t:l = %u:%u:%u:%u\n", device->os_info.scsiAddress.host, device->os_info.scsiAddress.channel, device->os_info.scsiAddress.target, device->os_info.scsiAddress.lun);

            printf("SG driver version = %u.%u.%u\n", device->os_info.sgDriverVersion.majorVersion, device->os_info.sgDriverVersion.minorVersion, device->os_info.sgDriverVersion.revision);
            #endif
            
            // Fill in all the device info.
            //this code to set up passthrough commands for USB and IEEE1394 has been removed for now to match Windows functionality. Need better intelligence than this.
            //Some of these old pass-through types issue vendor specific op codes that could be misinterpretted on some devices.
//              if (device->drive_info.interface_type == USB_INTERFACE || device->drive_info.interface_type == IEEE_1394_INTERFACE)
//              {
//                  //TODO: Actually get the VID and PID set before calling this...currently it just issues an identify command to test which passthrough to use until it works. - TJE
//                  set_ATA_Passthrough_Type_By_PID_and_VID(device);
//              }

            ret = fill_Drive_Info_Data(device);

            #if defined (_DEBUG)
            printf("\nvm helper\n");
            printf("Drive type: %d\n",device->drive_info.drive_type);
            printf("Interface type: %d\n",device->drive_info.interface_type);
            printf("Media type: %d\n",device->drive_info.media_type);
            #endif
        }
        safe_Free(deviceHandle);

    }
    else
    {
        rc = Nvme_GetAdapterList(&nvmeAdptList);
    
        if (rc != SUCCESS) 
        {
            return FAILURE;
        }
    
        #if defined (_DEBUG)
        printf("This is a NVMe drive\n");
        printf("Attempting to open %s\n", deviceHandle);
        #endif
        // Note: We are opening a READ/Write flag
        /**
         * Opening up the dev handle for NVMe 
         */
    
        device->os_info.nvmeFd = Nvme_Open(&nvmeAdptList, filename);
    
        /**
         * We should do a HDD/SSD open here
         */
    
        if (device->os_info.nvmeFd == NULL)
        {
            perror("open");
            device->os_info.nvmeFd = errno;
            printf("open failure\n");
            printf("Error: ");
            print_Errno_To_Screen(errno);
            if (device->os_info.nvmeFd == EACCES) 
            {
                safe_Free(deviceHandle);
                return PERMISSION_DENIED;
            }
            else
            {
                safe_Free(deviceHandle);
                return FAILURE;
            }
        }
    
        //Adding support for different device discovery options. 
        if (device->dFlags == OPEN_HANDLE_ONLY)
        {
            safe_Free(deviceHandle);
            return ret;
        }
        //\\TODO: Add support for other flags. 
    
        if ((device->os_info.nvmeFd != NULL) && (ret == SUCCESS))
        {
            #if defined (_DEBUG)
            printf("Getting SG driver version\n");
            #endif
    
            /**
             * Setting up NVMe drive blindly for now
             */
    
            device->drive_info.interface_type = NVME_INTERFACE;
            device->drive_info.drive_type = NVME_DRIVE;
            device->drive_info.media_type = MEDIA_NVM;
            memcmp(device->drive_info.T10_vendor_ident, "NVMe",4);
            device->os_info.osType = OS_ESX;
            memcpy(&(device->os_info.name), filename, strlen(filename) + 1);
    
            #if 0
            // Check we have a valid device by trying an ioctl
            // From http://tldp.org/HOWTO/SCSI-Generic-HOWTO/pexample.html
            if ((ioctl(device->os_info.fd, SG_GET_VERSION_NUM, &k) < 0) || (k < 30000))
            {
                printf("%s: SG_GET_VERSION_NUM on %s failed version=%d\n", __FUNCTION__, filename,k);
                perror("SG_GET_VERSION_NUM");
                close(device->os_info.fd);
            }
            else
            {
                //http://www.faqs.org/docs/Linux-HOWTO/SCSI-Generic-HOWTO.html#IDDRIVER
                device->os_info.sgDriverVersion.driverVersionValid = true;
                device->os_info.sgDriverVersion.majorVersion = (uint8_t)(k / 10000);
                device->os_info.sgDriverVersion.minorVersion = (uint8_t)((k - (device->os_info.sgDriverVersion.majorVersion * 10000)) / 100);
                device->os_info.sgDriverVersion.revision = (uint8_t)(k - (device->os_info.sgDriverVersion.majorVersion * 10000) - (device->os_info.sgDriverVersion.minorVersion * 100));
                
                //set the OS Type
                device->os_info.osType = OS_LINUX;
    
                //set scsi interface and scsi drive until we know otherwise
                device->drive_info.drive_type = SCSI_DRIVE;
                device->drive_info.interface_type = SCSI_INTERFACE;
                device->drive_info.media_type = MEDIA_HDD;
                //now have the device information fields set
                #if defined (_DEBUG)
                printf("Setting interface, drive type, secondary handles\n");
                #endif
                set_Device_Fields_From_Handle(deviceHandle, device);
    
                #if defined (_DEBUG)
                printf("name = %s\t friendly name = %s\n2ndName = %s\t2ndFName = %s\n",
                       device->os_info.name,
                       device->os_info.friendlyName,
                       device->os_info.secondName,
                       device->os_info.secondFriendlyName
                       );
                printf("h:c:t:l = %u:%u:%u:%u\n", device->os_info.scsiAddress.host, device->os_info.scsiAddress.channel, device->os_info.scsiAddress.target, device->os_info.scsiAddress.lun);
    
                printf("SG driver version = %u.%u.%u\n", device->os_info.sgDriverVersion.majorVersion, device->os_info.sgDriverVersion.minorVersion, device->os_info.sgDriverVersion.revision);
                #endif
            }
            #endif
                
            #if !defined(DISABLE_NVME_PASSTHROUGH)
            if (device->drive_info.interface_type == NVME_INTERFACE) 
            {
                #if 0
                ret = ioctl(device->os_info.fd, NVME_IOCTL_ID);
                if (ret < 0)
                {
                     perror("nvme_ioctl_id");
                     return ret;
                }
                device->drive_info.lunOrNSID = (uint32_t) ret;
                #endif
                ret = fill_In_NVMe_Device_Info(device);
            }
            else
            #endif
            {
                // Fill in all the device info.
                //this code to set up passthrough commands for USB and IEEE1394 has been removed for now to match Windows functionality. Need better intelligence than this.
                //Some of these old pass-through types issue vendor specific op codes that could be misinterpretted on some devices.
    //              if (device->drive_info.interface_type == USB_INTERFACE || device->drive_info.interface_type == IEEE_1394_INTERFACE)
    //              {
    //                  //TODO: Actually get the VID and PID set before calling this...currently it just issues an identify command to test which passthrough to use until it works. - TJE
    //                  set_ATA_Passthrough_Type_By_PID_and_VID(device);
    //              }
    
                #if 0
                ret = fill_Drive_Info_Data(device);
                #endif
            }
            #if defined (_DEBUG)
            printf("\nvm helper\n");
            printf("Drive type: %d\n",device->drive_info.drive_type);
            printf("Interface type: %d\n",device->drive_info.interface_type);
            printf("Media type: %d\n",device->drive_info.media_type);
            #endif
        }
        safe_Free(deviceHandle);
    }

    return ret;
}
//http://www.tldp.org/HOWTO/SCSI-Generic-HOWTO/scsi_reset.html
//sgResetType should be one of the values from the link above...so bus or device...controller will work but that shouldn't be done ever.
int sg_reset(int fd, int resetType)
{
    int ret = UNKNOWN;
    
    ret = ioctl(fd, SG_SCSI_RESET, &resetType);

    if (ret < 0)
    {
        #if defined(_DEBUG)
        printf("Reset failure! errorcode: %d, errno: %d\n",ret, errno);
        print_Errno_To_Screen(errno);
        #endif
        if (errno == EAFNOSUPPORT)
        {
            ret = NOT_SUPPORTED;
            /*
            scsiIoCtx->returnStatus.format = SCSI_SENSE_CUR_INFO_FIXED;
            scsiIoCtx->returnStatus.senseKey = 0x05;
            scsiIoCtx->returnStatus.asc = 0x20;
            scsiIoCtx->returnStatus.ascq = 0x00;
            //dummy up sense data
            if (scsiIoCtx->psense != NULL)
            {
                memset(scsiIoCtx->psense, 0, scsiIoCtx->senseDataSize);
                //fill in not supported
                scsiIoCtx->psense[0] = SCSI_SENSE_CUR_INFO_FIXED;
                scsiIoCtx->psense[2] = 0x05;
                //asc
                scsiIoCtx->psense[12] = 0x20;//invalid operation code
                //acsq
                scsiIoCtx->psense[13] = 0x00;
            }
            */
        }
        else
        {
            ret = FAILURE;
            /*
            scsiIoCtx->returnStatus.format = SCSI_SENSE_CUR_INFO_FIXED;
            scsiIoCtx->returnStatus.senseKey = 0x05;
            scsiIoCtx->returnStatus.asc = 0x24;
            scsiIoCtx->returnStatus.ascq = 0x00;
            //dummy up sense data
            if (scsiIoCtx->psense != NULL)
            {
                memset(scsiIoCtx->psense,0,scsiIoCtx->senseDataSize);
                //fill in not supported
                scsiIoCtx->psense[0] = SCSI_SENSE_CUR_INFO_FIXED;
                scsiIoCtx->psense[2] = 0x05;
                //asc
                scsiIoCtx->psense[12] = 0x24;//invalid field in CDB
                //acsq
                scsiIoCtx->psense[13] = 0x00;
            }
            */
        }
    }
    else
    {
        //poll for reset completion
        #if defined(_DEBUG)
        printf("Reset in progress, polling for completion!\n");
        #endif
        resetType = SG_SCSI_RESET_NOTHING;
        while (errno == EBUSY)
        {
            ret = ioctl(fd, SG_SCSI_RESET, &resetType);
        }
        ret = SUCCESS;
        //printf("Reset Success!\n");
    }
    return ret;
}

int device_Reset(int fd)
{
    return sg_reset(fd, SG_SCSI_RESET_DEVICE);
}

int bus_Reset(int fd)
{
    return sg_reset(fd, SG_SCSI_RESET_BUS);
}

int host_Reset(int fd)
{
    return sg_reset(fd, SG_SCSI_RESET_HOST);
}

int send_IO( ScsiIoCtx *scsiIoCtx )
{
    int ret = FAILURE;    
#ifdef _DEBUG
    printf("-->%s \n",__FUNCTION__);
#endif
    switch (scsiIoCtx->device->drive_info.interface_type)
    {
    case NVME_INTERFACE://send_IO only sends ATA and SCSI IOs, so if we are here, we must be sending a SCSI command, so just send an sg_io
        //USB, ATA, and SCSI interface all use sg, so just issue an SG IO.
    case SCSI_INTERFACE:
    case IDE_INTERFACE:
    case USB_INTERFACE:
    case IEEE_1394_INTERFACE:
        ret = send_sg_io(scsiIoCtx); 
        break;
    case RAID_INTERFACE:
        if (scsiIoCtx->device->issue_io != NULL)
        {
            ret = scsiIoCtx->device->issue_io(scsiIoCtx);
        }
        else
        {
            if (VERBOSITY_QUIET < scsiIoCtx->device->deviceVerbosity)
            {
                printf("No Raid PassThrough IO Routine present for this device\n");
            }
        }
        break;
    default:
        if (VERBOSITY_QUIET < scsiIoCtx->device->deviceVerbosity)
        {
            printf("Target Device does not have a valid interface %d\n",\
                        scsiIoCtx->device->drive_info.interface_type);
        }
        break;
    }
#ifdef _DEBUG
    printf("<--%s (%d)\n",__FUNCTION__, ret);
#endif
    return ret;
}

int send_sg_io( ScsiIoCtx *scsiIoCtx )
{
    sg_io_hdr_t io_hdr;
    uint8_t     sense_buffer[SPC3_SENSE_LEN] = { 0 };
    int         ret          = SUCCESS;
    seatimer_t  commandTimer;
#ifdef _DEBUG
    printf("-->%s \n",__FUNCTION__);
#endif


    memset(&commandTimer,0,sizeof(seatimer_t));
    //int idx = 0;
    memset(sense_buffer, 0, SPC3_SENSE_LEN);
    // Start with zapping the io_hdr
    memset(&io_hdr, 0, sizeof(sg_io_hdr_t));

    if (VERBOSITY_BUFFERS <= scsiIoCtx->device->deviceVerbosity)
    {
        printf("Sending command with send_IO\n");
    }

    // Set up the io_hdr
    io_hdr.interface_id = 'S';
    io_hdr.cmd_len = scsiIoCtx->cdbLength;
    // Use user's sense or local?
    if ((scsiIoCtx->senseDataSize) && (scsiIoCtx->psense != NULL))
    {
        io_hdr.mx_sb_len = scsiIoCtx->senseDataSize;
        io_hdr.sbp = scsiIoCtx->psense;
    }
    else
    {
        io_hdr.mx_sb_len = sizeof(sense_buffer);
        io_hdr.sbp = (unsigned char *)&sense_buffer;
    }

    switch (scsiIoCtx->direction)
    {
    case XFER_NO_DATA:
    case SG_DXFER_NONE:
        io_hdr.dxfer_direction = SG_DXFER_NONE;
        break;
    case XFER_DATA_IN:
    case SG_DXFER_FROM_DEV:
        io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
        break;
    case XFER_DATA_OUT:
    case SG_DXFER_TO_DEV:
        io_hdr.dxfer_direction = SG_DXFER_TO_DEV;
        break;
    case SG_DXFER_TO_FROM_DEV:
        io_hdr.dxfer_direction = SG_DXFER_TO_FROM_DEV;
        break;
        //case SG_DXFER_UNKNOWN:
        //io_hdr.dxfer_direction = SG_DXFER_UNKNOWN;
        //break;
    default:
        if (VERBOSITY_QUIET < scsiIoCtx->device->deviceVerbosity)
        {
            printf("%s Didn't understand direction\n", __FUNCTION__);
        }
        return BAD_PARAMETER;
    }

    io_hdr.dxfer_len = scsiIoCtx->dataLength;
    io_hdr.dxferp = scsiIoCtx->pdata;
    io_hdr.cmdp = scsiIoCtx->cdb;
    if (scsiIoCtx->timeout != 0)
    {
        io_hdr.timeout = scsiIoCtx->timeout;
        //this check is to make sure on commands that set a very VERY large timeout (*cough* *cough* ata security) that we DON'T do a conversion and leave the time as the max...
        if (scsiIoCtx->timeout < 4294966)
        {
            io_hdr.timeout *= 1000;//convert to milliseconds
        }
    }
    else
    {
        io_hdr.timeout = 15 * 1000;
    }
    
    // \revisit: should this be FF or something invalid than 0?
    scsiIoCtx->returnStatus.format = 0xFF;
    scsiIoCtx->returnStatus.senseKey = 0;
    scsiIoCtx->returnStatus.asc = 0;
    scsiIoCtx->returnStatus.ascq = 0;
    //print_io_hdr(&io_hdr);
    //printf("scsiIoCtx->device->os_info.fd = %d\n", scsiIoCtx->device->os_info.fd);
    start_Timer(&commandTimer);
    ret = ioctl(scsiIoCtx->device->os_info.fd, SG_IO, &io_hdr);
    stop_Timer(&commandTimer);
    scsiIoCtx->device->os_info.last_error = errno;
    if (ret < 0)
    {
        ret = OS_PASSTHROUGH_FAILURE;
        if (VERBOSITY_COMMAND_VERBOSE <= scsiIoCtx->device->deviceVerbosity)
        {
            if (scsiIoCtx->device->os_info.last_error != 0)
            {
                printf("Error: ");
                print_Errno_To_Screen(scsiIoCtx->device->os_info.last_error);
            }
        }
    }

    //print_io_hdr(&io_hdr);

    if (io_hdr.sb_len_wr)
    {
        scsiIoCtx->returnStatus.format  = io_hdr.sbp[0];
        get_Sense_Key_ASC_ASCQ_FRU(io_hdr.sbp, io_hdr.mx_sb_len, &scsiIoCtx->returnStatus.senseKey, &scsiIoCtx->returnStatus.asc, &scsiIoCtx->returnStatus.ascq, &scsiIoCtx->returnStatus.fru);
    }

    // \todo shouldn't this be done at a higher level?
    if (((io_hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK) || // check info
        (io_hdr.masked_status != 0x00) ||                  // check status(0 if ioctl success)
        (io_hdr.msg_status != 0x00) ||                     // check message status
        (io_hdr.host_status != 0x00) ||                    // check host status
        (io_hdr.driver_status != 0x00))                   // check driver status
    {
        if (scsiIoCtx->verbose)
        {
            printf(" info 0x%x\n maskedStatus 0x%x\n msg_status 0x%x\n host_status 0x%x\n driver_status 0x%x\n",\
                       io_hdr.info, io_hdr.masked_status, io_hdr.msg_status, io_hdr.host_status,\
                       io_hdr.driver_status);


            decipher_maskedStatus(io_hdr.masked_status);

            //if (io_hdr.driver_status & SG_ERR_DRIVER_SENSE)
            if ((io_hdr.driver_status & 0x08) && (io_hdr.sb_len_wr))
            {
                print_Data_Buffer( (uint8_t *)io_hdr.sbp, io_hdr.sb_len_wr, true );
            }
        }
    }
    scsiIoCtx->device->drive_info.lastCommandTimeNanoSeconds = get_Nano_Seconds(commandTimer);
#ifdef _DEBUG
    printf("<--%s (%d)\n",__FUNCTION__, ret);
#endif
    return ret;
}

static int nvme_filter( const struct dirent *entry)
{
    int nvmeHandle = strncmp("nvme",entry->d_name,4);
    if (nvmeHandle != 0)
    {
        return !nvmeHandle;
    }
    if (strlen(entry->d_name) > 5)
    {
        char* partition = strpbrk(entry->d_name,"p");
        if(partition != NULL)
        {
            return nvmeHandle;
        }
        else
        {
            return !nvmeHandle;
        }
    }
    else
    {
        return 0;
    }
}

//-----------------------------------------------------------------------------
//
//  get_Device_Count()
//
//! \brief   Description:  Get the count of devices in the system that this library
//!                        can talk to. This function is used in conjunction with
//!                        get_Device_List, so that enough memory is allocated.
//
//  Entry:
//!   \param[out] numberOfDevices = integer to hold the number of devices found. 
//!   \param[in] flags = eScanFlags based mask to let application control. 
//!                      NOTE: currently flags param is not being used.  
//!
//  Exit:
//!   \return SUCCESS - pass, !SUCCESS fail or something went wrong
//
//-----------------------------------------------------------------------------
int get_Device_Count(uint32_t * numberOfDevices, uint64_t flags)
{
    int  num_devs = 0, num_nvme_devs = 0;
    int rc;
    struct nvme_adapter_list nvmeAdptList;

    struct dirent **namelist;

    num_devs = scandir("/dev/disks", &namelist, drive_filter, alphasort); 
    /*
    if(num_devs == 0)
    {
        //check for SD devices
        num_devs = scandir("/dev", &namelist, sd_filter, alphasort); 
    } 
    */ 

#ifdef _DEBUG
    printf("get_Device_Count : num_devs %d\n", num_devs);
#endif

    //add nvme devices to the list
    rc = Nvme_GetAdapterList(&nvmeAdptList);

    if(rc == SUCCESS) 
    {
        num_nvme_devs = nvmeAdptList.count;
    }

#ifdef _DEBUG
    printf("get_Device_Count : num_nvme_devs %d\n", num_nvme_devs);
#endif

    *numberOfDevices = num_devs + num_nvme_devs;
    
    return SUCCESS;
}

//-----------------------------------------------------------------------------
//
//  get_Device_List()
//
//! \brief   Description:  Get a list of devices that the library supports. 
//!                        Use get_Device_Count to figure out how much memory is
//!                        needed to be allocated for the device list. The memory 
//!                        allocated must be the multiple of device structure. 
//!                        The application can pass in less memory than needed 
//!                        for all devices in the system, in which case the library 
//!                        will fill the provided memory with how ever many device 
//!                        structures it can hold. 
//  Entry:
//!   \param[out] ptrToDeviceList = pointer to the allocated memory for the device list
//!   \param[in]  sizeInBytes = size of the entire list in bytes. 
//!   \param[in]  versionBlock = versionBlock structure filled in by application for 
//!                              sanity check by library. 
//!   \param[in] flags = eScanFlags based mask to let application control. 
//!                      NOTE: currently flags param is not being used.  
//!
//  Exit:
//!   \return SUCCESS - pass, !SUCCESS fail or something went wrong
//
//-----------------------------------------------------------------------------
int get_Device_List(tDevice * const ptrToDeviceList, uint32_t sizeInBytes, versionBlock ver, uint64_t flags)
{
    int returnValue = SUCCESS;
    int numberOfDevices = 0;
    int driveNumber = 0, found = 0, failedGetDeviceCount = 0;
    char name[128] = { 0 }; //Because get device needs char
    int fd;
    tDevice * d = NULL;
    struct nvme_adapter_list nvmeAdptList;
    int rc;
#if defined (DEGUG_SCAN_TIME)
    seatimer_t getDeviceTimer;
    seatimer_t getDeviceListTimer;
    memset(&getDeviceTimer, 0, sizeof(seatimer_t));
    memset(&getDeviceListTimer, 0, sizeof(seatimer_t));
#endif
    struct dirent **namelist;

    int  num_sg_devs = 0, num_sd_devs = 0, num_nvme_devs = 0;

    num_sg_devs = scandir("/dev/disks", &namelist, drive_filter, alphasort); 
    if(num_sg_devs == 0)
    {
        //check for SD devices
        num_sd_devs = scandir("/dev/disks", &namelist, drive_filter, alphasort); 
    }

    rc = Nvme_GetAdapterList(&nvmeAdptList);

    if (rc == SUCCESS) 
    {
        num_nvme_devs = nvmeAdptList.count;
    }


    char **devs = (char **)calloc(MAX_DEVICES_TO_SCAN, sizeof(char *));
    int i = 0, j = 0;
    //add sg/sd devices to the list
    for (; i < (num_sg_devs + num_sd_devs); i++)
    {
        devs[i] = (char *)malloc((strlen("/dev/disks/") + strlen(namelist[i]->d_name) + 1) * sizeof(char));
        strcpy(devs[i], "/dev/disks/");
        strcat(devs[i], namelist[i]->d_name);
        free(namelist[i]);
    }
    safe_Free(namelist);

    //add nvme devices to the list
    for (j = 0; i < (num_sg_devs + num_sd_devs + num_nvme_devs) && i < MAX_DEVICES_PER_CONTROLLER;i++, j++)
    {
        int nvmeAdptNameLen = 0;

        nvmeAdptNameLen = strlen(nvmeAdptList.adapters[j].name);
        devs[i] = (char *)malloc((nvmeAdptNameLen + 1) * sizeof(char));
        memset(devs[i], 0, (nvmeAdptNameLen + 1) * sizeof(char));
        strcpy(devs[i], nvmeAdptList.adapters[j].name);
    }
    devs[i] = NULL; //Added this so the for loop down doesn't cause a segmentation fault.


    //TODO: Check if sizeInBytes is a multiple of 
    if (!(ptrToDeviceList) || (!sizeInBytes))
    {
        returnValue = BAD_PARAMETER;
    }
    else if ((!(validate_Device_Struct(ver))))
    {
        returnValue = LIBRARY_MISMATCH;
    }
    else
    {
        numberOfDevices = sizeInBytes / sizeof(tDevice);
        d = ptrToDeviceList;
#if defined (DEGUG_SCAN_TIME)
        start_Timer(&getDeviceListTimer);
#endif
        for (driveNumber = 0; ((driveNumber < MAX_DEVICES_PER_CONTROLLER && driveNumber < (num_sg_devs + num_sd_devs + num_nvme_devs)) || (found < numberOfDevices)); driveNumber++)
        {
            if(strlen(devs[driveNumber]) == 0)
            {
                continue;
            }
            memset(name, 0, sizeof(name));//clear name before reusing it
            strncpy(name, devs[driveNumber], M_Min(sizeof(name), strlen(devs[driveNumber])));
            eVerbosityLevels temp = d->deviceVerbosity;
            memset(d, 0, sizeof(tDevice));
            d->deviceVerbosity = temp;
            d->sanity.size = ver.size;
            d->sanity.version = ver.version;
#if defined (DEGUG_SCAN_TIME)
            seatimer_t getDeviceTimer;
            memset(&getDeviceTimer, 0, sizeof(seatimer_t));
            start_Timer(&getDeviceTimer);
#endif
            d->dFlags = flags;
            returnValue = get_Device(name, d);
#if defined (DEGUG_SCAN_TIME)
            stop_Timer(&getDeviceTimer);
            printf("Time to get %s = %fms\n", name, get_Milli_Seconds(getDeviceTimer));
#endif
            if (returnValue != SUCCESS)
            {
                failedGetDeviceCount++;
            }
            found++;
            d++;
        }
#if defined (DEGUG_SCAN_TIME)
        stop_Timer(&getDeviceListTimer);
        printf("Time to get all device = %fms\n", get_Milli_Seconds(getDeviceListTimer));
#endif
        if (found == failedGetDeviceCount)
        {
            returnValue = FAILURE;
        }
        else if (failedGetDeviceCount)
        {
            returnValue = WARN_NOT_ALL_DEVICES_ENUMERATED;
        }
    }
    safe_Free(devs);
    return returnValue;
}

//-----------------------------------------------------------------------------
//
//  close_Device()
//
//! \brief   Description:  Given a device, close it's handle. 
//
//  Entry:
//!   \param[in] device = device stuct that holds device information. 
//!
//  Exit:
//!   \return SUCCESS - pass, !SUCCESS fail or something went wrong
//
//-----------------------------------------------------------------------------
int close_Device(tDevice *dev)
{
    int retValue = 0;
    if (dev)
    {
        Nvme_Close(dev->os_info.nvmeFd);
        dev->os_info.last_error = errno;

        return SUCCESS;
    }
    else
    {
        return MEMORY_FAILURE;
    }
}

#if !defined(DISABLE_NVME_PASSTHROUGH)
int send_NVMe_IO(nvmeCmdCtx *nvmeIoCtx )
{
    int ret = 0;//NVME_SC_SUCCESS;//This defined value used to exist in some version of nvme.h but is missing in nvme_ioctl.h...it was a value of zero, so this should be ok.
    struct usr_io uio;

#ifdef _DEBUG
    printf("-->%s\n",__FILE__);
    printf("-->%s\n",__FUNCTION__);
#endif

    memset(&uio, 0, sizeof(uio));

    if ( nvmeIoCtx == NULL )
    {
#ifdef _DEBUG
    printf("-->%s\n",__FUNCTION__);
#endif
        return BAD_PARAMETER; 
    }

    switch (nvmeIoCtx->commandType) 
    {
    case NVM_ADMIN_CMD:
        memcpy(&(uio.cmd), &(nvmeIoCtx->cmd.adminCmd), sizeof(nvmeCommands));
        
        /*        
        uio.cmd.header.opCode = nvmeIoCtx->cmd.adminCmd.opcode;
        uio.cmd.header.fusedOp = nvmeIoCtx->cmd.adminCmd.flags;
        uio.cmd.header.namespaceID = nvmeIoCtx->cmd.adminCmd.nsid;
        uio.cmd.header.metadataPtr = nvmeIoCtx->cmd.adminCmd.metadata;
        uio.cmd.header.prp[0].addr = nvmeIoCtx->cmd.adminCmd.addr;
        uio.cmd.header.prp[1].lower = nvmeIoCtx->cmd.adminCmd.metadataLen;
        uio.cmd.header.prp[1].upper = nvmeIoCtx->cmd.adminCmd.dataLen;
        uio.cmd.cmd.vendorSpecific.buffNumDW = nvmeIoCtx->cmd.adminCmd.cdw10;
        uio.cmd.cmd.vendorSpecific.metaNumDW = nvmeIoCtx->cmd.adminCmd.cdw11;
        uio.cmd.cmd.vendorSpecific.vndrCDW12 = nvmeIoCtx->cmd.adminCmd.cdw12;
        uio.cmd.cmd.vendorSpecific.vndrCDW13 = nvmeIoCtx->cmd.adminCmd.cdw13;
        uio.cmd.cmd.vendorSpecific.vndrCDW14 = nvmeIoCtx->cmd.adminCmd.cdw14;
        uio.cmd.cmd.vendorSpecific.vndrCDW15 = nvmeIoCtx->cmd.adminCmd.cdw15;
        */

        if((nvmeIoCtx->commandDirection == XFER_NO_DATA) ||
           (nvmeIoCtx->commandDirection == XFER_DATA_IN))
        {
            uio.direction = XFER_FROM_DEV;
        }
        else
        {
            uio.direction = XFER_TO_DEV;
        }

        uio.length = nvmeIoCtx->dataSize;
        uio.addr = (vmk_uint32)nvmeIoCtx->cmd.adminCmd.addr;
        uio.namespaceID = nvmeIoCtx->cmd.adminCmd.nsid;
        uio.timeoutUs = nvmeIoCtx->timeout ? nvmeIoCtx->timeout * 1000 : 15000;

#ifdef _DEBUG
/*
        printf("Before Nvme_AdminPassthru %s: uio.addr=%p, uio.length=%d, cdw10=0x%X, nsid=0x%x\n",\
               __FUNCTION__, uio.addr,\
               uio.length, uio.cmd.cmd.vendorSpecific.buffNumDW,\
               uio.cmd.header.namespaceID\
                );
*/
        printf("Before Nvme_AdminPassthru %s: uio.length=%d, cdw10=0x%X, nsid=0x%x\n",\
               __FUNCTION__,\
               uio.length, uio.cmd.cmd.vendorSpecific.buffNumDW,\
               uio.cmd.header.namespaceID\
                );

        /*
        printf("Printing uio.cmd\n");
        for(int i = 0; i < sizeof(uio.cmd); i++) 
        {
            if(i%8 == 0) 
            {
                printf("%d : ", i);
            }
            printf(" %x", (unsigned char)(*((unsigned char *)&(uio.cmd) + i)));
            if(i%8 == 7) 
            {
                printf("\n");
            }
        }
        printf("\n"); 
        */ 

#endif

        ret = Nvme_AdminPassthru(nvmeIoCtx->device->os_info.nvmeFd, &uio);

#ifdef _DEBUG
/*
        printf("After Nvme_AdminPassthru %s: uio.addr=%p, uio.length=%d, cdw10=0x%X, nsid=0x%x\n",\
               __FUNCTION__, uio.addr,\
               uio.length, uio.cmd.cmd.vendorSpecific.buffNumDW,\
               uio.cmd.header.namespaceID\
                );

        printf("After Nvme_AdminPassthru %s: uio.length=%d, cdw10=0x%X, nsid=0x%x\n",\
               __FUNCTION__,\
               uio.length, uio.cmd.cmd.vendorSpecific.buffNumDW,\
               uio.cmd.header.namespaceID\
                );

        printf("Printing buffer\n");
        for(int i = 0; i < uio.length; i++) 
        {
            if(i%8 == 0) 
            {
                printf("%d : ", i);
            }
            printf(" %x", (unsigned char)(*((unsigned char *)uio.addr + i)));
            if(i%8 == 7) 
            {
                printf("\n");
            }
        }
        printf("\n");
*/
#endif


        nvmeIoCtx->device->os_info.last_error = ret;
        //Get error? 
        if (ret < 0) 
        {
            if (VERBOSITY_QUIET < nvmeIoCtx->device->deviceVerbosity)
            {
                perror("send_IO");
            }
        }
        nvmeIoCtx->commandCompletionData.commandSpecific = uio.comp.param.cmdSpecific;
        nvmeIoCtx->commandCompletionData.dw0Valid = true;
        nvmeIoCtx->commandCompletionData.dw1Reserved = uio.comp.reserved;
        nvmeIoCtx->commandCompletionData.dw1Valid = true;
        nvmeIoCtx->commandCompletionData.sqIDandHeadPtr = M_WordsTo4ByteValue(uio.comp.sqID, uio.comp.sqHdPtr);
        nvmeIoCtx->commandCompletionData.dw2Valid = true;
        nvmeIoCtx->commandCompletionData.statusAndCID = uio.comp.cmdID | (uio.comp.phaseTag << 16) | (uio.comp.SC << 17) | (uio.comp.SCT << 25) | (uio.comp.more << 30) | (uio.comp.noRetry << 31);
        nvmeIoCtx->commandCompletionData.dw3Valid = true;
        break;

    #if 0
    case NVM_CMD:
        memset(&nvmCmd,0,sizeof(nvmCmd));
        ret = ioctl(nvmeIoCtx->device->os_info.fd, NVME_IOCTL_SUBMIT_IO, &nvmCmd);
        break;
    #endif
            
    default:
        return BAD_PARAMETER;
        break;
    }
    if (VERBOSITY_COMMAND_VERBOSE <= nvmeIoCtx->device->deviceVerbosity)
    {
        if (nvmeIoCtx->device->os_info.last_error != 0)
        {
            printf("Error: ");
            print_Errno_To_Screen(nvmeIoCtx->device->os_info.last_error);
        }
    }
#ifdef _DEBUG
    printf("<--%s (%d)\n",__FUNCTION__, ret);
#endif
    return ret;
}

int nvme_Reset(tDevice *device)
{
    //This is a stub. If this is possible, this should perform an nvme reset;
    return OS_COMMAND_NOT_AVAILABLE;
}

int nvme_Subsystem_Reset(tDevice *device)
{
    //This is a stub. If this is possible, this should perform an nvme subsystem reset;
    return OS_COMMAND_NOT_AVAILABLE;
}

//Case to remove this from sg_helper.h/c and have a platform/lin/pci-herlper.h vs platform/win/pci-helper.c 

int pci_Read_Bar_Reg( tDevice * device, uint8_t * pData, uint32_t dataSize )
{
    int ret = UNKNOWN;
    int fd=0;
    void * barRegs = NULL;
    char sysfsPath[PATH_MAX];
    sprintf(sysfsPath,"/sys/block/%s/device/resource0",device->os_info.name);
    fd = open(sysfsPath, O_RDONLY);
    if (fd >= 0) 
    {
        //
        barRegs = mmap(0,dataSize,PROT_READ, MAP_SHARED, fd, 0);
        if (barRegs != MAP_FAILED) 
        {
            ret = SUCCESS;
            memcpy(pData,barRegs,dataSize);
        }
        else
        {
            ret = FAILURE;
        }
        close(fd);
    }
    else
    {
        if (VERBOSITY_QUIET < device->deviceVerbosity)
        {
            printf("couldn't open device %s\n", device->os_info.name);
        }
        ret = BAD_PARAMETER;
    }
    return ret;
}
#endif
int os_Read(tDevice *device, uint64_t lba, bool async, uint8_t *ptrData, uint32_t dataSize)
{
    return NOT_SUPPORTED;
}

int os_Write(tDevice *device, uint64_t lba, bool async, uint8_t *ptrData, uint32_t dataSize)
{
    return NOT_SUPPORTED;
}

int os_Verify(tDevice *device, uint64_t lba, uint32_t range)
{
    return NOT_SUPPORTED;
}

int os_Flush(tDevice *device)
{
    return NOT_SUPPORTED;
}
