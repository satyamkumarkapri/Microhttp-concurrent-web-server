/**
 * script.js — MICROHTTP Demo Website JavaScript
 *
 * Minimal JavaScript for the architecture tab switcher.
 * No external libraries — pure vanilla JS.
 */

'use strict';

/**
 * showArch — switch the visible architecture diagram tab.
 * @param {string} name - 'threadpool', 'epoll', or 'connection'
 */
function showArch(name) {
    /* Hide all arch content panels */
    document.querySelectorAll('.arch-content').forEach(el => {
        el.classList.add('hidden');
    });

    /* Deactivate all tab buttons */
    document.querySelectorAll('.tab-btn').forEach(btn => {
        btn.classList.remove('active');
    });

    /* Show the selected panel */
    const panel = document.getElementById('arch-' + name);
    if (panel) panel.classList.remove('hidden');

    /* Find and activate the corresponding button */
    document.querySelectorAll('.tab-btn').forEach(btn => {
        if (btn.getAttribute('onclick') === `showArch('${name}')`) {
            btn.classList.add('active');
        }
    });
}

/**
 * Smooth scroll to section on internal link click.
 */
document.querySelectorAll('a[href^="#"]').forEach(anchor => {
    anchor.addEventListener('click', function (e) {
        const target = document.querySelector(this.getAttribute('href'));
        if (target) {
            e.preventDefault();
            target.scrollIntoView({ behavior: 'smooth', block: 'start' });
        }
    });
});

/**
 * Simple intersection observer to animate cards on scroll.
 */
const observer = new IntersectionObserver((entries) => {
    entries.forEach(entry => {
        if (entry.isIntersecting) {
            entry.target.style.opacity = '1';
            entry.target.style.transform = 'translateY(0)';
        }
    });
}, { threshold: 0.1 });

/* Apply initial state and observe cards */
document.querySelectorAll('.mode-card, .feature-item, .concept-card').forEach(card => {
    card.style.opacity = '0';
    card.style.transform = 'translateY(20px)';
    card.style.transition = 'opacity 0.5s ease, transform 0.5s ease';
    observer.observe(card);
});

/* Display server info in console for demonstration */
console.log('%cMICROHTTP', 'color: #63b3ed; font-size: 24px; font-weight: 800');
console.log('%cConcurrent HTTP/1.1 Web Server', 'color: #718096; font-size: 14px');
console.log('%cBuilt with C11, Linux syscalls — no frameworks.', 'color: #4a5568; font-size: 12px');
