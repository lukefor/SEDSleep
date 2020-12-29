Description
==
SEDSleep is a Windows storage filter driver that transparently unlocks NVMe drives encrypted with [sedutil](https://github.com/Drive-Trust-Alliance/sedutil) when resuming from S3 sleep mode.

Status
===
Working reliably, but with caveats:

 - **Security:** The unlock commands, including hashed password, are hard-coded into the driver .sys file.
 - **Security:** The usual warning that silent decrypting on resume from S3 sleep, especially without TPM involvement, is not very secure - i.e. attacker can reboot machine from login screen and access all your data. You can use Group Policy to prevent some (all?) methods of rebooting from the login screen.
 - **Data Loss:** This could cause data loss, use at your own risk.
 - **Multiple disks:** This blindly applies to ALL disks. Harmless to USB flash drives, USB hard drives, SD cards and virtual filesystems as far as I can tell, YMMV for other types.
 - **Old SHA1 hash:** This uses the original DTA SHA1 code. Newer forks with different hashing may run into problems.
 - **Risky install:** If anything goes wrong with the driver build or installation, your windows installation will be unbootable, even in safe mode (as this is a storage related driver). Have a means of using regedit (to disable the driver) externally handy, such as a second windows installation.

Building
==

 1. Build this hacky branch of sedutil https://github.com/lukefor/sedutil/tree/windows_sleep, which we will use to extract the raw unlock commands. If you are using a fork with a non-SHA1 hashing algorithm, you will need to manually integrate the changes in the following commit to the fork you are using: https://github.com/lukefor/sedutil/commit/dd60185ab76d1ae928c47ecb9b12b87265c05d6a
 2. Run `--setlockingrange 0 rw password \\.\PhysicalDrive0`
 3. `xxd -i send5.bin send5.h`
 4. `xxd -i send7.bin send7.h`
 5. `xxd -i send9.bin send9.h` (if you don't have a send9.bin, see here: https://github.com/lukefor/sedutil/issues/1)
 6. Copy .h files to this project dir
 7. Run `--sedmbrdone on password \\.\PhysicalDrive0`
 8. `xxd -i send7.bin send7mbr.h`
 9. Rename variables within to `send7mbr_bin` and `send7mbr_bin_len`
 10. Copy `send7mbr.h` to project dir  
 11. Build, sign and install driver. See here for more info: https://github.com/lukefor/sedutil/issues/1
 12. Draw the rest of the owl
 

To-do
===
 - Make the gathering of send5/7/9/7mbr.h less horrendous
 - (or) Receive unlock data from usermode
	 - Avoids needing end users to compile from source 
	 - Would improve security (although if saving to disk, system32 folder may be hard to beat permission-wise..?)
 - Generally tidy up code and remove more of DiskPerf sample code
 - Investigate whether a FOSS license is possible given sample code basis
 - Support SATA drives
 - Jump through signing hoops to ship a compiled version
 



