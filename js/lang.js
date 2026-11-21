// Türkçe / English: dil değiştirirken aynı bölümde kal.
// When switching language, keep the reader in the same section: the other page uses the same ids.
(function () {
  var link = document.querySelector('[data-lang-switch]');
  if (!link) return;
  var base = link.getAttribute('href');
  link.addEventListener('click', function () {
    var id = '';
    var sections = document.querySelectorAll('main section[id]');
    for (var i = 0; i < sections.length; i++) {
      if (sections[i].getBoundingClientRect().top <= 120) id = sections[i].id;
    }
    link.setAttribute('href', base + (id ? '#' + id : ''));
  });
})();
