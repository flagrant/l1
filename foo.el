(defun output-file (file)
  (let ((sp (split-string file "\\.src")))
    (if (not (= (length sp) 2))
	(error "invalid file name: %s" file))
    (apply 'concat sp)))

(defun doit (file)
  (interactive "fFile: ")
  (let* ((nfile (expand-file-name "~/src/l1/amd64.tests.h"))
	 (buf (find-file-noselect nfile))
	 form result omit (n 0))
    (save-excursion
      (set-buffer buf)
      (erase-buffer)
      (insert-file-contents file)
      (goto-char (point-min))
      (while (re-search-forward "{ \\(.*\\)\; }.*\\(\".*\"\\)" nil t)
	(setq form (cons (match-string 1) form))
	(setq result (cons (match-string 2) result))
	(save-excursion
	  (beginning-of-line)
	  (setq omit (cons (if (looking-at "//") t nil) omit))))
      (erase-buffer)
      (setq form (reverse form))
      (setq result (reverse result))
      (setq omit (reverse omit))
      (while form
	(let ((o (pop omit)) (f (pop form)) (r (pop result)))
	  (or o
	       (insert (format "%s\ttest( %60s,   %-50s );\t// %3d\n" (if o "//" "") f r n)))
	  (setq n (1+ n))))
      (save-buffer 0))
    (kill-buffer buf)))