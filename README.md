# BackupHelper

Original plugin: https://github.com/YQ-LL-Plugins/BackupHelper  
A LeviLamina plugin which is port from LiteLoaderBDS to help you backup more easily

## Download

```
lip install github.com/ShrBox/BackupHelper
```

## Usage

`/backup` - Start backup now

`/backup list` - List all backups existing

`/backup recover [number]` - Select recover to a certain backup version, and restart server to finish recover process

`/backup reload` - Reload config file

These commands can be executed at BDS console, or by OPs in game.



## Config file

At `plugins/BackupHelper/config.ini`, with comments in file

```ini
[Main]
; 备份存档保存的最长时间，单位：天
MaxStorageTime=7

; 备份文件夹位置
BackupPath=.\backup

; 备份文件压缩等级，可选等级有0,1,3,5,7,9
; 默认为0，即仅打包
Compress=0

; 等待压缩的最长时间，单位：秒，如果为0则无限等待
MaxWaitForZip=1800
```
