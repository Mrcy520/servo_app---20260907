param(
    [Parameter(Mandatory = $true)][string]$InputPdf,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [int]$Width = 2526
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime

$script:AsTaskGeneric = [System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object {
        $_.Name -eq 'AsTask' -and $_.IsGenericMethodDefinition -and
        $_.GetParameters().Count -eq 1
    } |
    Select-Object -First 1

$script:AsTaskAction = [System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object {
        $_.Name -eq 'AsTask' -and -not $_.IsGenericMethod -and
        $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction'
    } |
    Select-Object -First 1

function Wait-WinRtResult {
    param($Operation, [Type]$ResultType)
    $task = $script:AsTaskGeneric.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    $task.Wait()
    return $task.Result
}

function Wait-WinRtAction {
    param($Operation)
    $task = $script:AsTaskAction.Invoke($null, @($Operation))
    $task.Wait()
}

$storageFileType = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$pdfDocumentType = [Windows.Data.Pdf.PdfDocument, Windows.Data.Pdf, ContentType = WindowsRuntime]
$null = [Windows.Storage.Streams.InMemoryRandomAccessStream, Windows.Storage.Streams, ContentType = WindowsRuntime]
$null = [Windows.Data.Pdf.PdfPageRenderOptions, Windows.Data.Pdf, ContentType = WindowsRuntime]

$inputPath = [System.IO.Path]::GetFullPath($InputPdf)
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($outputPath) | Out-Null

$storageFile = Wait-WinRtResult ([Windows.Storage.StorageFile]::GetFileFromPathAsync($inputPath)) $storageFileType
$document = Wait-WinRtResult ([Windows.Data.Pdf.PdfDocument]::LoadFromFileAsync($storageFile)) $pdfDocumentType

for ($index = 0; $index -lt $document.PageCount; $index++) {
    $page = $document.GetPage($index)
    try {
        $memoryStream = New-Object Windows.Storage.Streams.InMemoryRandomAccessStream
        $options = New-Object Windows.Data.Pdf.PdfPageRenderOptions
        $options.DestinationWidth = [uint32]$Width
        Wait-WinRtAction ($page.RenderToStreamAsync($memoryStream, $options))

        $memoryStream.Seek(0)
        $dotNetStream = [System.IO.WindowsRuntimeStreamExtensions]::AsStreamForRead($memoryStream)
        try {
            $target = Join-Path $outputPath ('source-page-{0}.png' -f ($index + 1))
            $fileStream = [System.IO.File]::Create($target)
            try {
                $dotNetStream.CopyTo($fileStream)
            }
            finally {
                $fileStream.Dispose()
            }
        }
        finally {
            $dotNetStream.Dispose()
            $memoryStream.Dispose()
        }
    }
    finally {
        $page.Dispose()
    }
}

Write-Output ("Rendered {0} pages to {1}" -f $document.PageCount, $outputPath)
