#!/usr/bin/perl

use Test::More tests => 75;
use LWP::UserAgent;
use Archive::Zip;

$temp_zip_path = "/tmp/mod_zip.zip";
$http_root = "http://localhost:8081";

sub read_file {
    $file = shift;
    undef $/;

    open( FILE, "<", $file );
    $contents = <FILE>;
    close( FILE );

    return $contents;
}

sub write_temp_zip {
    my $content = shift;

    open TEMPFILE, ">", $temp_zip_path;
    print TEMPFILE $content;
    close TEMPFILE;

    return Archive::Zip->new($temp_zip_path);
}

sub test_zip_archive {
    my ($content, $condition) = @_;

    my $zip = write_temp_zip($content);

    my $zip_file1_content = $zip->contents( 'file1.txt' );
    my $zip_file2_content = $zip->contents( 'file2.txt' );

    my $file1_content = read_file("nginx/html/file1.txt");
    my $file2_content = read_file("nginx/html/file2.txt");

    is($zip_file1_content, $file1_content, "file1.txt in the ZIP $condition");
    is($zip_file2_content, $file2_content, "file2.txt in the ZIP $condition");

    return $zip;
}

$ua = LWP::UserAgent->new;

$zip_length = 303;

$file1_content = read_file("nginx/html/file1.txt");
$file2_content = read_file("nginx/html/file2.txt");

$file1_offset = 51;
$file2_offset = 127;


########## Dowload component files

$response = $ua->get("$http_root/file1.txt");

is( $response->content, $file1_content, "download file1.txt" );
is( length($file1_content), 24, "file1.txt file size" );

$response = $ua->get("$http_root/file1.txt", "Range" => "bytes=0-3");
is( $response->content, "This", "download file1.txt partial" );

$response = $ua->get("$http_root/file2.txt");
is( $response->content, $file2_content, "download file2.txt" );
is( length($file2_content), 25, "file2.txt file size" );

########## Download ZIP files

$response = $ua->get("$http_root/zip-missing-crc.txt");
is($response->code, 200, "Returns OK with missing CRC");
like($response->header("Content-Length"), qr/^\d+$/, "Content-Length header when missing CRC");
is($response->header("Accept-Ranges"), undef, "No Accept-Ranges header when missing CRC");

$zip = test_zip_archive($response->content, "when missing CRC");
is($zip->memberNamed("file1.txt")->hasDataDescriptor(), 8, "Has data descriptor when missing CRC");
is($zip->memberNamed("file1.txt")->crc32String(), "1a6349c5", "Generated file1.txt CRC is correct");
is($zip->memberNamed("file2.txt")->crc32String(), "5d70c4d3", "Generated file2.txt CRC is correct");

$response = $ua->get("$http_root/zip-404.txt");
is($response->code, 500, "Server error with bad file");

$response = $ua->get("$http_root/zip-many-files.txt");
is($response->code, 200, "Returns OK with many files");

$zip = test_zip_archive($response->content, "with many files");
is($zip->numberOfMembers(), 136, "Correct number in many-file ZIP");

$response = $ua->get("$http_root/zip-local-files.txt");
is($response->code, 200, "Returns OK with local files");

$zip = test_zip_archive($response->content, "with local files");
is($zip->numberOfMembers(), 2, "Correct number in local-file ZIP");
is($zip->memberNamed("file1.txt")->crc32String(), "1a6349c5", "Generated file1.txt CRC is correct (local)");
is($zip->memberNamed("file2.txt")->crc32String(), "5d70c4d3", "Generated file2.txt CRC is correct (local)");

open LARGEFILE, ">", "nginx/html/largefile.txt";
for (0..99999) {
    print LARGEFILE "X" x 99;
    print LARGEFILE "\n";
}
close LARGEFILE;

$response = $ua->get("$http_root/zip-large-file.txt");
is($response->code, 200, "Returns OK with large file");

$zip = write_temp_zip($response->content);
$large_zip_file_content = $zip->contents( 'largefile.txt' );
$large_file_content = read_file("nginx/html/largefile.txt");
is(length($large_zip_file_content), length($large_file_content), "Found large file in ZIP");

unlink "nginx/html/largefile.txt";

$response = $ua->get("$http_root/zip.txt");
is( $response->header( "X-Archive-Files" ), "zip", 
    "X-Archive-Files header" );
is( $response->header( "Content-Type" ), "application/zip", 
    "Content-Type header" );
is( $response->header( "Cache-Control" ), "max-age=0",
    "Cache-Control header" );
is( $response->header( "Accept-Ranges" ), "bytes",
    "Accept-Ranges header" );
is( $response->header( "Content-Length" ), $zip_length,
    "Content-Length header" );

$zip = test_zip_archive($response->content);
is($zip->memberNamed("file1.txt")->hasDataDescriptor(), 0, "No data descriptor when CRC supplied");

########### Byte-range

$response = $ua->get("$http_root/zip.txt", "Range" => "bytes=10000-10001");
is($response->code, 416, "Request range not satisfiable");

$response = $ua->get("$http_root/zip.txt", "Range" => "bytes=0-1");
is($response->code, 206, "206 Partial Content");
is(length($response->content), 2, "Length of partial content");
is($response->content, "PK", "Subrange at beginning");

$response = $ua->get("$http_root/zip.txt", "Range" => "bytes=30-38");
is($response->code, 206, "206 Partial Content");
is(length($response->content), 9, "Length of partial content");
is($response->content, "file1.txt", "Subrange in a file header");

$response = $ua->get("$http_root/zip.txt", 
    "Range" => "bytes=".($file2_offset+1)."-152");
is($response->code, 206, "206 Partial Content");
is(length($response->content), 25, "Length of partial content");
is($response->content, "This is the second file.\n", "Subrange of included file");

$response = $ua->get("$http_root/zip.txt", 
    "Range" => "bytes=".($file2_offset+1)."-".($file2_offset+4));
is($response->code, 206, "206 Partial Content");
is(length($response->content), 4, "Length of partial content");
is($response->content, "This", "Subrange is a subrange of included file");

$response = $ua->get("$http_root/zip.txt", 
    "Range" => "bytes=".($file2_offset+3)."-".($file2_offset+4));
is($response->code, 206, "206 Partial Content");
is(length($response->content), 2, "Length of partial content");
is($response->content, "is", "Subrange is a subrange of included file");

# Subrange including all of first file and part of second.
$response = $ua->get("$http_root/zip.txt", "Range" => "bytes=0-".($file2_offset+5));
open TMPFILE, ">", "/tmp/partial.zip";
print TMPFILE $response->content;
close TMPFILE;
is($response->code, 206, "206 Partial Content");
is(length($response->content), $file2_offset+1+5, "Length of partial content");
is(substr($response->content, $file2_offset+1, 4), "This", "Subrange spanning multiple files");

# Subrange including part of first and second files.
$response = $ua->get("$http_root/zip.txt", "Range" => "bytes=".($file1_offset+9)."-".($file2_offset+4));
open TMPFILE, ">", "/tmp/partial.zip";
print TMPFILE $response->content;
close TMPFILE;
is($response->code, 206, "206 Partial Content");
is(length($response->content), ($file2_offset+4)-($file1_offset+9)+1, "Length of partial content");
is(substr($response->content, 0, 14), "the first file", "Subrange spanning part of first file");
is(substr($response->content, 68, 4), "This", "Subrange spanning part of second file");

$response = $ua->get("$http_root/zip-local-files.txt", "Range" => "bytes=".($file1_offset+9)."-".($file2_offset+4));
open TMPFILE, ">", "/tmp/partial.zip";
print TMPFILE $response->content;
close TMPFILE;
is($response->code, 206, "206 Partial Content");
is(length($response->content), ($file2_offset+4)-($file1_offset+9)+1, "Length of partial content");
is(substr($response->content, 0, 14), "the first file", "Subrange spanning part of first file");
is(substr($response->content, 68, 4), "This", "Subrange spanning part of second file");

########### Prefix & suffix

$response = $ua->get("$http_root/zip.txt", "Range" => "bytes=".($file2_offset+1)."-");
is($response->code, 206, "206 Partial Content");
is(length($response->content), $zip_length - ($file2_offset+1), "Length of partial content (prefix)");
is(substr($response->content, 0, 25), "This is the second file.\n", "Subrange with prefix notation");

$response = $ua->get("$http_root/zip.txt", "Range" => "bytes=-".($zip_length-($file2_offset+1)));
is($response->code, 206, "206 Partial Content");
is(length($response->content), $zip_length - ($file2_offset+1), "Length of partial content (suffix)");
is(substr($response->content, 0, 25), "This is the second file.\n", "Subrange with suffix notation");

########### Multiple byte-ranges

$response = $ua->get("$http_root/zip.txt", 
    "Range" => "bytes=0-1,".($file2_offset+1)."-".($file2_offset+4));
is($response->code, 206, "206 Partial Content");
like($response->header("Content-Type"), qr(multipart/byteranges), "Content-Type: multipart/byteranges");
($boundary) = $response->header("Content-Type") =~ /multipart\/byteranges; boundary=(\w+)/;
like($response->content, 
    qr(--$boundary\r\nContent-Type: application/zip\r\nContent-Range: bytes 0-1/$zip_length\r\n\r\nPK\r\n), 
    "First chunk");
like($response->content, 
    qr(--$boundary\r\nContent-Type: application/zip\r\nContent-Range: bytes 128-131/$zip_length\r\n\r\nThis\r\n), 
    "Second chunk");

$response = $ua->get("$http_root/zip.txt",
    "Range" => "bytes=".($file2_offset+1)."-".($file2_offset+4).",0-1");
is($response->code, 206, "206 Partial Content");
like($response->header("Content-Type"), qr(multipart/byteranges), "Content-Type: multipart/byteranges");
($boundary) = $response->header("Content-Type") =~ /multipart\/byteranges; boundary=(\w+)/;
like($response->content, 
    qr(--$boundary\r\nContent-Type: application/zip\r\nContent-Range: bytes 128-131/$zip_length\r\n\r\nThis\r\n), 
    "First chunk");
like($response->content, 
    qr(--$boundary\r\nContent-Type: application/zip\r\nContent-Range: bytes 0-1/$zip_length\r\n\r\nPK\r\n), 
    "Second chunk");

