;;; emacs-webview2.el --- webview2 in Emacs -*- lexical-binding: t; -*-

;; Copyright (C) 2026 include-yy <yy@egh0bww1.com>

;; Author: include-yy <yy@egh0bww1.com>
;; Maintainer: include-yy <yy@egh0bww1.com>
;; Created: 2026-02-17 21:53+0800

;; Package-Version: 0.1
;; Package-Requires: ((emacs "31"))
;; Keywords: tools, html
;; URL: https://github.com/include-yy/emacs-webview2

;; SPDX-License-Identifier: GPL-3.0-or-later

;; emacs-webview2 is free software; you can redistribute it and/or
;; modify it under the terms of the GNU General Public License as
;; published by the Free Software Foundation, either version 3 of the
;; License, or (at your option) any later version.

;; emacs-webview2 is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with emacs-webview2.  If not, see <https://www.gnu.org/licenses/>.

;;; Commentary:

;;; Code:
(require 'jsonrpc)
(require 'cl-lib)

(cl-defstruct t--webview
  "WebView2 instance wrapper."
  (id -1 :documentation "WebView2 object's id.")
  (frame nil :documentation "Frame object belongs to.")
  (url nil :documentation "Link navigate to.")
  (buffer nil :documentation "buffer the object belongs to."))

(defvar-local t--webview nil
  "Buffer-local Webview2 structure.")

(defconst t--dir
  (if (not load-in-progress) default-directory
    (file-name-directory load-file-name))
  "Package's root directory.")

(defvar t--conn nil
  "JSONRPC connection object to WebView2 manager process.")

(defvar t--buffer-registry nil
  "Managed buffers.")

(defvar t--shutting-down nil)

(defun t--alive-p ()
  "Check if the connection is alive"
  (and (jsonrpc-process-connection-p t--conn)
       (not t--shutting-down)
       (jsonrpc-running-p t--conn)))

(defun t--srpc (method params)
  (jsonrpc-request t--conn method params))

(defun t--say (method params)
  (jsonrpc-notify t--conn method params))

(defun t--get-frame-hwnd (&optional frame)
  "Get frame's Win32 HWND."
  (let ((id (frame-parameter (or frame (selected-frame)) 'window-id)))
    (string-to-number id)))

(defun t--get-window-rect (&optional window)
  "Get WINDOW bound rect."
  (cl-coerce (window-body-pixel-edges window) 'vector))

(defun m-exit ()
  (setq t--shutting-down t)
  (t--srpc 'exit [nil]))

(defun m-new (rect &optional url)
  (let ((hwnd (t--get-frame-hwnd)))
    (t--srpc 'new `[,hwnd ,rect ,url])))

(defun m-close (id)
  (t--srpc 'close `[,id]))

(defun m-resize (id rect)
  (t--srpc 'resize `[,id ,rect]))

(defun m-reparent (id hwnd)
  (t--srpc 'reparent `[,id ,hwnd]))

(defun m-visible-p (id)
  (t--srpc 'visible-p `[,id]))

(defun m-set-visible (id b)
  (let ((bool (if b t :json-false)))
    (t--srpc 'set-visible `[,id ,bool])))

(defun m-set-focus ()
  (t--srpc 'set-focus `[,(t--get-frame-hwnd)]))

(defun m-get-title (id)
  (t--srpc 'get-title `[,id]))

(defun t-set-focus-on-click ()
  (when (eq this-command 'mouse-drag-region)
    (when (t--alive-p)
      (m-set-focus))))

(defun t-monitor-window-configuration-change ()
  (when (t--alive-p)
    (let ((processed-ids))
      (dolist (wnd (window-list))
        (with-selected-window wnd
          (when-let* ((w t--webview)
                      (id (t--webview-id w)))
            (unless (memq id processed-ids)
              (push id processed-ids)
              (let* ((bounds (t--get-window-rect))
                     (frame (t--webview-frame w))
                     (hwnd (t--get-frame-hwnd frame))
                     (curr-hwnd (t--get-frame-hwnd)))
                (unless (eq hwnd curr-hwnd)
                  (m-reparent id curr-hwnd)
                  (setf (t--webview-frame w) (window-frame wnd)))
                (m-resize id bounds)
                (m-set-visible id t))))))
      (dolist (buf t--buffer-registry)
        (when (buffer-live-p buf)
          (with-current-buffer buf
            (when-let* ((_ (not (get-buffer-window buf t)))
                        (w t--webview)
                        (id (t--webview-id w)))
              (m-set-visible id nil))))))))

(defun t-after-frame-delete (_frame)
  (t--monitor-window-configuration-change))

(defun t-rescue-webview-on-frame-delete (frame)
  (when (t--alive-p)
    (dolist (wnd (window-list frame))
      (with-current-buffer (window-buffer wnd)
        (when-let* ((w t--webview)
                    (_ (eq (t--webview-frame w) frame))
                    (safe-frame (car (remove frame (frame-list))))
                    (hwnd (t--get-frame-hwnd safe-frame))
                    (id (t--webview-id w)))
          (m-reparent id hwnd)
          (m-set-visible id nil)
          (setf (t--webview-frame w) safe-frame))))))

(defun t--register-hooks ()
  (add-hook 'pre-command-hook #'t-set-focus-on-click)
  (add-hook 'delete-frame-functions
            #'t-rescue-webview-on-frame-delete)
  (add-hook 'window-configuration-change-hook
            #'t-monitor-window-configuration-change)
  (add-hook 'after-delete-frame-functions
            #'t-after-frame-delete))

(defun t--unregister-hooks ()
  (remove-hook 'pre-command-hook #'t-set-focus-on-click)
  (remove-hook 'delete-frame-functions
               #'t-rescue-webview-on-frame-delete)
  (remove-hook 'window-configuration-change-hook
               #'t-monitor-window-configuration-change)
  (remove-hook 'after-delete-frame-functions
               #'t-after-frame-delete))

(defun t--cleanup-sentinel (_conn)
  (setq t--shutting-down t)
  (dolist (buf (copy-sequence t--buffer-registry))
    (when (buffer-live-p buf)
      (kill-buffer buf)))
  (t--unregister-hooks)
  (setq t--buffer-registry nil)
  (setq t--conn nil))

(defun t--start-webview2-manager ()
  "Start the Manager Subprocess and create the RPC connection."
  (when-let* ((_ (not (t--alive-p)))
              (path (file-name-concat t--dir "x64" "Debug" "wv2.exe"))
              (proc (make-process :name "WebView2-Manager"
                                  :command `(,path)
                                  :coding 'binary)))
    (setq t--conn (make-instance 'jsonrpc-process-connection
                                 :name "Emacs-WebView2"
                                 :process proc
                                 :on-shutdown #'t--cleanup-sentinel))
    (setq t--shutting-down nil)
    (t--register-hooks)))

(defun t--on-kill-buffer ()
  (when (and (t--alive-p) (t--webview-p t--webview))
    (let ((id (t--webview-id t--webview)))
      (m-close id)))
  (setq t--buffer-registry
        (delq (current-buffer) t--buffer-registry)))

(defun t--setup-buffer (obj)
  (setq t--webview obj)
  (add-to-list 't--buffer-registry (current-buffer))
  (add-hook 'kill-buffer-hook 't--on-kill-buffer nil t))

(defun t-open-url (url)
  (interactive "sUrl: ")
  (t--start-webview2-manager)
  (when (string-empty-p url)
    (setq url "https://google.com"))
  (let ((buffer (generate-new-buffer "EWV2")))
    (switch-to-buffer buffer)
    (let* ((rect (t--get-window-rect))
           (id (m-new rect url))
           (wobj (make-emacs-webview2--webview
                  :id id :frame (selected-frame)
                  :url url :buffer buffer)))
      (with-current-buffer buffer
        (t--setup-buffer wobj)))))

(defun t-shutdown ()
  (interactive)
  (when (t--alive-p) (m-exit)))

;; Local Variables:
;; read-symbol-shorthands: (("t-" . "emacs-webview2-")
;;                          ("m-" . "emacs-webview2--wv-"))
;; coding: utf-8-unix
;; End:
