$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert | Where-Object { $_.Subject -like "*Pixel Mirroring Developer*" } | Select-Object -First 1

if (-not $cert) {
    Write-Host "Erstelle neues Code-Signing-Zertifikat..."
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=Pixel Mirroring Developer" -CertStoreLocation "Cert:\CurrentUser\My"
}

Write-Host "Zertifikat gefunden: $($cert.Thumbprint)"

# Exportieren & in vertrauenswürdige Stammzertifizierungsstellen importieren
$certPath = "$env:TEMP\PixelMirroring.cer"
Export-Certificate -Cert $cert -FilePath $certPath | Out-Null
Import-Certificate -FilePath $certPath -CertStoreLocation "Cert:\CurrentUser\Root" | Out-Null
Import-Certificate -FilePath $certPath -CertStoreLocation "Cert:\CurrentUser\TrustedPublisher" | Out-Null

# EXE signieren falls vorhanden
$exePath = "c:\Pixel-Mirroring\Client\build\Release\PixelMirroring.exe"
if (Test-Path $exePath) {
    Write-Host "Signiere PixelMirroring.exe..."
    Set-AuthenticodeSignature -FilePath $exePath -Certificate $cert
}

# Installer signieren falls vorhanden
$installerPath = "c:\Pixel-Mirroring\Client\build\PixelMirroring-3.67.0-win64.exe"
if (Test-Path $installerPath) {
    Write-Host "Signiere Installer..."
    Set-AuthenticodeSignature -FilePath $installerPath -Certificate $cert
}
