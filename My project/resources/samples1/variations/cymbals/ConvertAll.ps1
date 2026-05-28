$folder = "e:\ShapeShiftDrums_repository\ShapeShiftDrums\My project\resources\samples1\variations\cymbals\"
$script = "e:\ShapeShiftDrums_repository\ShapeShiftDrums\My project\resources\samples1\bin2c.py"
$outFolder = Join-Path $folder "generated"

if (!(Test-Path $outFolder)) {
    New-Item -ItemType Directory -Path $outFolder | Out-Null
}

Get-ChildItem $folder -File -Filter "*.wav" | ForEach-Object {
    $inputFile = $_.FullName

    # Имя файла БЕЗ _data
    $baseName = $_.BaseName -replace '[^A-Za-z0-9_]', '_'

    # Имя массива ВНУТРИ cpp/h С _data
    $arrayName = $baseName + "_data"

    # Выходные файлы остаются без _data
    $cppFile = Join-Path $outFolder ($baseName + ".cpp")
    $hFile   = Join-Path $outFolder ($baseName + ".h")

    py $script $cppFile $hFile $inputFile $arrayName

    Write-Host "Created:" $baseName".cpp / "$baseName".h with array "$arrayName
}