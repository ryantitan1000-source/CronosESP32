' Abre el panel del molinete como una "app": arranca el servidor (oculto) y abre una ventana propia.
' Doble clic para usarlo. Si ya estaba abierto, solo abre otra ventana.
' Busca solo el Python instalado (el de versión más alta en %LOCALAPPDATA%\Programs\Python) y la placa por USB.
Set sh = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
carpeta = fso.GetParentFolderName(WScript.ScriptFullName)

py = "": mejor = -1
base = sh.ExpandEnvironmentStrings("%LOCALAPPDATA%") & "\Programs\Python"
If fso.FolderExists(base) Then
  For Each f In fso.GetFolder(base).SubFolders
    If fso.FileExists(f.Path & "\pythonw.exe") Then
      On Error Resume Next
      ver = CLng(Mid(f.Name, 7))
      If Err.Number <> 0 Then ver = 0: Err.Clear
      On Error GoTo 0
      If ver > mejor Then mejor = ver: py = f.Path & "\pythonw.exe"
    End If
  Next
End If
If py = "" Then py = "pythonw.exe"

sh.Run """" & py & """ """ & carpeta & "\tools\dashboard.py"" --com auto", 0, False
WScript.Sleep 2000
url = "http://127.0.0.1:8765"
edge1 = sh.ExpandEnvironmentStrings("%ProgramFiles(x86)%") & "\Microsoft\Edge\Application\msedge.exe"
edge2 = sh.ExpandEnvironmentStrings("%ProgramFiles%") & "\Microsoft\Edge\Application\msedge.exe"
If fso.FileExists(edge1) Then
  sh.Run """" & edge1 & """ --app=" & url & " --window-size=1100,800", 1, False
ElseIf fso.FileExists(edge2) Then
  sh.Run """" & edge2 & """ --app=" & url & " --window-size=1100,800", 1, False
Else
  sh.Run url, 1, False
End If
