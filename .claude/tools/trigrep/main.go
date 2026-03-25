package main

import (
	"bufio"
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
)

// Index format (binary):
// Header: magic(4) + version(4) + numFiles(4) + numTrigrams(4)
// FileTable: for each file: pathLen(2) + path bytes
// TrigramTable: sorted array of [trigram(3) + pad(1) + offset(4) + count(4)] = 12 bytes each
// PostingLists: arrays of uint32 file IDs

const magic = 0x54524749 // "TRGI"
const version = 1

func extractTrigrams(s string) []uint32 {
	seen := make(map[uint32]bool)
	b := []byte(s)
	for i := 0; i+3 <= len(b); i++ {
		t := uint32(b[i])<<16 | uint32(b[i+1])<<8 | uint32(b[i+2])
		seen[t] = true
	}
	result := make([]uint32, 0, len(seen))
	for t := range seen {
		result = append(result, t)
	}
	return result
}

// extractLiterals extracts required literal substrings from a regex pattern.
// Returns a list of literal strings that MUST appear in any match.
// Uses a simple heuristic: find the longest unescaped literal runs.
func extractLiterals(pattern string) []string {
	var literals []string
	var current strings.Builder
	escaped := false
	inClass := false

	flush := func() {
		s := current.String()
		if len(s) >= 3 {
			literals = append(literals, s)
		}
		current.Reset()
	}

	for i := 0; i < len(pattern); i++ {
		c := pattern[i]
		if escaped {
			escaped = false
			switch c {
			case 'd', 'w', 's', 'D', 'W', 'S', 'b', 'B', 'n', 't', 'r':
				flush()
			default:
				current.WriteByte(c)
			}
			continue
		}
		if c == '\\' {
			escaped = true
			continue
		}
		if inClass {
			if c == ']' {
				inClass = false
				flush()
			}
			continue
		}
		switch c {
		case '[':
			flush()
			inClass = true
		case '.', '*', '+', '?', '(', ')', '|', '^', '$', '{', '}':
			flush()
		default:
			current.WriteByte(c)
		}
	}
	flush()

	// Sort by length descending, take top 3 longest
	sort.Slice(literals, func(i, j int) bool {
		return len(literals[i]) > len(literals[j])
	})
	if len(literals) > 3 {
		literals = literals[:3]
	}
	return literals
}

type buildCmd struct {
	dir    string
	output string
	exts   string
	ignore string
}

func (b *buildCmd) run() error {
	start := time.Now()

	extSet := make(map[string]bool)
	for _, e := range strings.Split(b.exts, ",") {
		e = strings.TrimSpace(e)
		if e != "" {
			extSet["."+e] = true
		}
	}

	ignoreSet := make([]string, 0)
	for _, ig := range strings.Split(b.ignore, ",") {
		ig = strings.TrimSpace(ig)
		if ig != "" {
			ignoreSet = append(ignoreSet, ig)
		}
	}

	// Collect files
	var files []string
	err := filepath.Walk(b.dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return nil
		}
		if info.IsDir() {
			name := info.Name()
			if name == ".git" || name == ".claude" {
				return filepath.SkipDir
			}
			for _, ig := range ignoreSet {
				if strings.Contains(path, ig) {
					return filepath.SkipDir
				}
			}
			return nil
		}
		if len(extSet) > 0 && !extSet[filepath.Ext(path)] {
			return nil
		}
		for _, ig := range ignoreSet {
			if strings.Contains(path, ig) {
				return nil
			}
		}
		files = append(files, path)
		return nil
	})
	if err != nil {
		return err
	}

	fmt.Fprintf(os.Stderr, "Indexing %d files...\n", len(files))

	// Build trigram -> []fileID posting lists
	posting := make(map[uint32][]uint32)

	for i, fpath := range files {
		if i%1000 == 0 && i > 0 {
			fmt.Fprintf(os.Stderr, "  %d/%d files processed\n", i, len(files))
		}
		f, err := os.Open(fpath)
		if err != nil {
			continue
		}
		content, err := io.ReadAll(io.LimitReader(f, 1<<20)) // max 1MB per file
		f.Close()
		if err != nil {
			continue
		}
		seen := make(map[uint32]bool)
		for j := 0; j+3 <= len(content); j++ {
			t := uint32(content[j])<<16 | uint32(content[j+1])<<8 | uint32(content[j+2])
			if !seen[t] {
				seen[t] = true
				posting[t] = append(posting[t], uint32(i))
			}
		}
	}

	// Sort trigrams for binary search
	trigrams := make([]uint32, 0, len(posting))
	for t := range posting {
		trigrams = append(trigrams, t)
	}
	sort.Slice(trigrams, func(i, j int) bool { return trigrams[i] < trigrams[j] })

	// Write index
	if err := os.MkdirAll(filepath.Dir(b.output), 0755); err != nil {
		return err
	}
	out, err := os.Create(b.output)
	if err != nil {
		return err
	}
	defer out.Close()
	w := bufio.NewWriter(out)

	write32 := func(v uint32) {
		var buf [4]byte
		binary.LittleEndian.PutUint32(buf[:], v)
		w.Write(buf[:])
	}
	write16 := func(v uint16) {
		var buf [2]byte
		binary.LittleEndian.PutUint16(buf[:], v)
		w.Write(buf[:])
	}

	// Header
	write32(magic)
	write32(version)
	write32(uint32(len(files)))
	write32(uint32(len(trigrams)))

	// File table
	for _, fpath := range files {
		rel, _ := filepath.Rel(b.dir, fpath)
		write16(uint16(len(rel)))
		w.WriteString(rel)
	}

	// Calculate posting list offsets
	// Each trigram entry: trigram(4) + offset(4) + count(4) = 12 bytes
	headerSize := 4 * 4 // 4 uint32s
	fileTableSize := 0
	for _, fpath := range files {
		rel, _ := filepath.Rel(b.dir, fpath)
		fileTableSize += 2 + len(rel)
	}
	trigramTableSize := len(trigrams) * 12
	postingStart := uint32(headerSize + fileTableSize + trigramTableSize)

	// Trigram table
	offset := postingStart
	for _, t := range trigrams {
		write32(t)
		write32(offset)
		write32(uint32(len(posting[t])))
		offset += uint32(len(posting[t])) * 4
	}

	// Posting lists
	for _, t := range trigrams {
		for _, fid := range posting[t] {
			write32(fid)
		}
	}

	if err := w.Flush(); err != nil {
		return err
	}

	fmt.Fprintf(os.Stderr, "Index built in %v: %d files, %d unique trigrams -> %s\n",
		time.Since(start).Round(time.Millisecond), len(files), len(trigrams), b.output)
	return nil
}

type index struct {
	files    []string
	trigrams []uint32 // sorted
	offsets  []uint32
	counts   []uint32
	data     []byte
}

func loadIndex(path string) (*index, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	if len(data) < 16 {
		return nil, fmt.Errorf("index too small")
	}
	if binary.LittleEndian.Uint32(data[0:]) != magic {
		return nil, fmt.Errorf("invalid index magic")
	}
	numFiles := int(binary.LittleEndian.Uint32(data[8:]))
	numTrigrams := int(binary.LittleEndian.Uint32(data[12:]))

	pos := 16
	files := make([]string, numFiles)
	for i := range files {
		if pos+2 > len(data) {
			return nil, fmt.Errorf("index corrupt at file table")
		}
		l := int(binary.LittleEndian.Uint16(data[pos:]))
		pos += 2
		if pos+l > len(data) {
			return nil, fmt.Errorf("index corrupt at file path")
		}
		files[i] = string(data[pos : pos+l])
		pos += l
	}

	trigrams := make([]uint32, numTrigrams)
	offsets := make([]uint32, numTrigrams)
	counts := make([]uint32, numTrigrams)
	for i := 0; i < numTrigrams; i++ {
		if pos+12 > len(data) {
			return nil, fmt.Errorf("index corrupt at trigram table")
		}
		trigrams[i] = binary.LittleEndian.Uint32(data[pos:])
		offsets[i] = binary.LittleEndian.Uint32(data[pos+4:])
		counts[i] = binary.LittleEndian.Uint32(data[pos+8:])
		pos += 12
	}

	return &index{files: files, trigrams: trigrams, offsets: offsets, counts: counts, data: data}, nil
}

func (idx *index) lookup(t uint32) []uint32 {
	lo, hi := 0, len(idx.trigrams)
	for lo < hi {
		mid := (lo + hi) / 2
		if idx.trigrams[mid] < t {
			lo = mid + 1
		} else {
			hi = mid
		}
	}
	if lo >= len(idx.trigrams) || idx.trigrams[lo] != t {
		return nil
	}
	off := idx.offsets[lo]
	cnt := idx.counts[lo]
	result := make([]uint32, cnt)
	for i := uint32(0); i < cnt; i++ {
		result[i] = binary.LittleEndian.Uint32(idx.data[off+i*4:])
	}
	return result
}

func intersect(a, b []uint32) []uint32 {
	var result []uint32
	i, j := 0, 0
	for i < len(a) && j < len(b) {
		if a[i] == b[j] {
			result = append(result, a[i])
			i++
			j++
		} else if a[i] < b[j] {
			i++
		} else {
			j++
		}
	}
	return result
}

type searchCmd struct {
	indexPath string
	pattern   string
	filesOnly bool
	context   int
	rootDir   string
}

func (s *searchCmd) run() error {
	idx, err := loadIndex(s.indexPath)
	if err != nil {
		return fmt.Errorf("loading index: %w", err)
	}

	re, err := regexp.Compile(s.pattern)
	if err != nil {
		return fmt.Errorf("invalid pattern: %w", err)
	}

	// Extract required literals from pattern
	literals := extractLiterals(s.pattern)

	var candidates []uint32
	if len(literals) == 0 {
		// No literals: must scan all files
		candidates = make([]uint32, len(idx.files))
		for i := range candidates {
			candidates[i] = uint32(i)
		}
	} else {
		// For each literal, get its trigrams and intersect
		// Between literals we intersect (all required)
		// Within a literal, all its trigrams are required (AND)
		for li, lit := range literals {
			ts := extractTrigrams(lit)
			if len(ts) == 0 {
				continue
			}
			// Intersect all trigrams within this literal
			var litCandidates []uint32
			for ti, t := range ts {
				posts := idx.lookup(t)
				if ti == 0 {
					litCandidates = posts
				} else {
					litCandidates = intersect(litCandidates, posts)
				}
				if len(litCandidates) == 0 {
					break
				}
			}
			// Intersect across literals
			if li == 0 {
				candidates = litCandidates
			} else {
				candidates = intersect(candidates, litCandidates)
			}
			if len(candidates) == 0 {
				break
			}
		}
	}

	fmt.Fprintf(os.Stderr, "Candidates: %d / %d files\n", len(candidates), len(idx.files))

	// Verify candidates with actual regex
	rootDir := s.rootDir
	if rootDir == "" {
		rootDir = "."
	}

	for _, fid := range candidates {
		fpath := filepath.Join(rootDir, idx.files[fid])
		f, err := os.Open(fpath)
		if err != nil {
			continue
		}

		if s.filesOnly {
			// Quick scan: just check if file matches
			content, err := io.ReadAll(f)
			f.Close()
			if err != nil {
				continue
			}
			if re.Match(content) {
				fmt.Println(idx.files[fid])
			}
		} else {
			// Line-by-line with context
			scanner := bufio.NewScanner(f)
			var lines []string
			for scanner.Scan() {
				lines = append(lines, scanner.Text())
			}
			f.Close()

			printed := false
			for lineNo, line := range lines {
				if re.MatchString(line) {
					if !printed {
						fmt.Printf("\n%s\n", idx.files[fid])
						printed = true
					}
					start := lineNo - s.context
					if start < 0 {
						start = 0
					}
					end := lineNo + s.context + 1
					if end > len(lines) {
						end = len(lines)
					}
					for i := start; i < end; i++ {
						if i == lineNo {
							fmt.Printf("%d: %s\n", i+1, lines[i])
						} else {
							fmt.Printf("%d- %s\n", i+1, lines[i])
						}
					}
					fmt.Println("--")
				}
			}
		}
	}

	return nil
}

func main() {
	buildFlags := flag.NewFlagSet("build", flag.ExitOnError)
	buildDir := buildFlags.String("dir", ".", "directory to index")
	buildOutput := buildFlags.String("output", ".claude/trigram-index.bin", "output index file")
	buildExts := buildFlags.String("ext", "cpp,h,java,py,go,rs,ts,js,c,cc,hpp,hh", "file extensions to index")
	buildIgnore := buildFlags.String("ignore", "vendor,node_modules,target,build", "directory patterns to ignore")

	searchFlags := flag.NewFlagSet("search", flag.ExitOnError)
	searchIndex := searchFlags.String("index", ".claude/trigram-index.bin", "index file")
	searchPattern := searchFlags.String("pattern", "", "regex pattern")
	searchFilesOnly := searchFlags.Bool("files-only", false, "only print matching file paths")
	searchContext := searchFlags.Int("context", 2, "lines of context")
	searchRoot := searchFlags.String("root", ".", "root directory for resolving file paths")

	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, "Usage: trigrep <build|search> [options]")
		os.Exit(1)
	}

	switch os.Args[1] {
	case "build":
		buildFlags.Parse(os.Args[2:])
		cmd := &buildCmd{dir: *buildDir, output: *buildOutput, exts: *buildExts, ignore: *buildIgnore}
		if err := cmd.run(); err != nil {
			fmt.Fprintln(os.Stderr, "Error:", err)
			os.Exit(1)
		}
	case "search":
		searchFlags.Parse(os.Args[2:])
		if *searchPattern == "" {
			fmt.Fprintln(os.Stderr, "Error: --pattern required")
			os.Exit(1)
		}
		cmd := &searchCmd{
			indexPath: *searchIndex,
			pattern:   *searchPattern,
			filesOnly: *searchFilesOnly,
			context:   *searchContext,
			rootDir:   *searchRoot,
		}
		if err := cmd.run(); err != nil {
			fmt.Fprintln(os.Stderr, "Error:", err)
			os.Exit(1)
		}
	default:
		fmt.Fprintln(os.Stderr, "Unknown command:", os.Args[1])
		os.Exit(1)
	}
}
