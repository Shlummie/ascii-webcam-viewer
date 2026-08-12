Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")

scriptDir = fso.GetParentFolderName(WScript.ScriptFullName)
exePath = scriptDir & "\ASCIICAM.exe"

If fso.FileExists(exePath) Then
    shell.Run """" & exePath & """", 0, False
Else
    cmd = "cmd.exe /c """ & scriptDir & "\run_asciicam_console.bat"""
    shell.Run cmd, 0, False
End If
