Description
==
SEDSleep is a Windows storage filter driver that transparently unlocks NVMe drives encrypted with [sedutil](https://github.com/Drive-Trust-Alliance/sedutil) when resuming from S3 sleep mode.

Status
===
Working, but with caveats:

 - **Security:** The unlock commands, including hashed password, are hard-coded into the driver .sys file.
 - **Security:** The usual warning that silent decrypting on resume from S3 sleep, especially without TPM involvement, is not very secure - i.e. attacker can reboot machine from login screen and access all your data.
 - **Data Loss:** This could very much cause data loss, use at your own risk.
 - Not tested when more than one disk is present.

Building
==

 1. Build this hacky branch of sedutil https://github.com/lukefor/sedutil/tree/windows_sleep, which we will use to extract the raw unlock commands
 2. Run --setlockingrange 0 rw password \\.\PhysicalDrive0
 3. xxd -i send5.bin send5.h
 4. xxd -i send7.bin send7.h
 5. xxd -i send9.bin send9.h
 6. Copy .h files to this project dir
 7. Run --sedmbrdone on password \\.\PhysicalDrive0
 8. xxd -i send7.bin send7mbr.h
 9. Rename variables within to send7mbr_bin and send7mbr_bin_len
 10. Copy send7mbr.h to project dir  
 11. Build, sign and install driver
 12. Draw the rest of the owl
 

To-do
===
 - Make the gathering of send5/7/9/7mbr.h less horrendous
 - (or) Receive unlock data from usermode
	 - Avoids needing end users to compile from source 
	 - Would improve security
 - Generally tidy up code and remove more of DiskPerf sample code
 - Investigate whether a FOSS license is possible given sample code basis
 - Support SATA drives
 - Jump through signing hoops to ship a compiled version
 



