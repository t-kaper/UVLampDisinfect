# How to Add 3D Files to This Repository

## Before You Start

If you cloned this repository on a new computer, make sure Git LFS is enabled:

```bash
git lfs install
```

## Step-by-Step Workflow for Adding New Files

### Step 1. Prepare Files in SolidWorks

1. **Keep related files together.** Store all `.sldprt` part files and `.sldasm` assembly files in the same project folder.
2. Before exporting to STL, use the **Binary** format whenever possible. It produces smaller files than ASCII STL.

### Step 2. Copy Files into the Repository

Copy the new `.sldprt`, `.sldasm`, or `.stl` files into the appropriate folder in the repository.

### Step 3. Check and Stage Files in the Terminal

1. Open a terminal in the project folder.
2. Check that Git sees the files and that no temporary files were added by mistake:

   ```bash
   git status
   ```

   If you see files starting with `~$`, close SolidWorks before continuing.

3. Stage the files:

   ```bash
   git add .
   ```

4. **Verify that Git LFS is tracking the large files:**

   ```bash
   git lfs status
   ```

   Your STL and SolidWorks files should appear as `LFS`-tracked files.

### Step 4. Commit and Push to GitHub

Run the final commands:

```bash
git commit -m "Add [part-or-assembly-name]"
git push origin main
```

Large files may take longer than usual to upload, so do not close the terminal until the push is complete.

## What You Should Not Do

- ❌ **Do not rename** `.sldprt` part files directly in Windows Explorer. That can break references inside the `.sldasm` assembly.
  Rename parts only from SolidWorks using the built-in rename tools.
- ❌ **Do not disable Git LFS.** Without it, GitHub may reject files larger than 100 MB.
