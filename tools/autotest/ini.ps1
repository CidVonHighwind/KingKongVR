# kkvr.ini editing for the test scripts (dot-sourced by run.ps1 and
# desktop_test.ps1).

# Sets $key=$value in [$section] of the ini text and returns the new text.
# Only that section is searched: a section's text runs from its header to the
# next header, so the same key in a later section is left alone. A missing key
# is added right below the header, a missing section at the end.
function Set-IniValue([string]$text, [string]$section, [string]$key, [string]$value) {
  $line = "$key=$value"
  # The match ends at "]": the line break stays with the header line.
  $header = [regex]::Match($text, "(?m)^\[" + [regex]::Escape($section) + "\](?=\r?$)")
  if (-not $header.Success) {
    return $text.TrimEnd() + "`r`n`r`n[$section]`r`n$line`r`n"
  }
  $bodyStart = $header.Index + $header.Length
  $next = [regex]::Match($text.Substring($bodyStart), "(?m)^\[")
  $bodyEnd = if ($next.Success) { $bodyStart + $next.Index } else { $text.Length }
  $body = $text.Substring($bodyStart, $bodyEnd - $bodyStart)
  $pattern = "(?m)^" + [regex]::Escape($key) + "=[^\r\n]*"
  if ($body -match $pattern) {
    $body = [regex]::Replace($body, $pattern, $line.Replace('$', '$$'))
  } else {
    $body = "`r`n$line" + $body
  }
  return $text.Substring(0, $bodyStart) + $body + $text.Substring($bodyEnd)
}
