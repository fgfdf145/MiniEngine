function Resolve-MiniEngineFormatCommand
{
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value
    )

    if ([System.IO.Path]::IsPathRooted($Value) -or
        $Value.Contains([System.IO.Path]::DirectorySeparatorChar) -or
        $Value.Contains([System.IO.Path]::AltDirectorySeparatorChar))
    {
        return [System.IO.Path]::GetFullPath($Value)
    }

    return $Value
}
