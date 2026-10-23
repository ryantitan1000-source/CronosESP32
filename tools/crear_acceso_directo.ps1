# Crea el acceso directo "Molinete" en el Escritorio y en el menú Inicio (abre Molinete.vbs).
# Uso (desde la carpeta del proyecto):  powershell -ExecutionPolicy Bypass -File tools\crear_acceso_directo.ps1
$raiz = Split-Path -Parent $PSScriptRoot
$vbs = Join-Path $raiz 'Molinete.vbs'
if (-not (Test-Path $vbs)) { Write-Error "No encuentro $vbs"; exit 1 }

$escritorio = [Environment]::GetFolderPath('Desktop')
$inicio = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'
$ws = New-Object -ComObject WScript.Shell
foreach ($carpeta in @($escritorio, $inicio)) {
  $lnk = Join-Path $carpeta 'Molinete.lnk'
  $s = $ws.CreateShortcut($lnk)
  $s.TargetPath = Join-Path $env:WINDIR 'System32\wscript.exe'
  $s.Arguments = '"' + $vbs + '"'
  $s.WorkingDirectory = $raiz
  $s.IconLocation = (Join-Path $env:WINDIR 'System32\imageres.dll') + ',100'
  $s.Description = 'Panel del molinete'
  $s.Save()
  Write-Output "Creado: $lnk"
}
